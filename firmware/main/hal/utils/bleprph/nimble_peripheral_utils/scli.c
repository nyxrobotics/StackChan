/*
 * SPDX-FileCopyrightText: 2021-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <ctype.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "driver/uart.h"
#include "esp_console.h"
#include "esp_err.h"
#include "esp_log.h"
#include "../bleprph.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define BLE_RX_TIMEOUT_MS 30000
#define CLI_EVENT_POLL_MS 100
#define CLI_STOP_TIMEOUT_MS 500
#define CLI_UART_NUM UART_NUM_0

typedef enum {
    SCLI_STATE_STOPPED,
    SCLI_STATE_STARTING,
    SCLI_STATE_RUNNING,
    SCLI_STATE_STOPPING,
    SCLI_STATE_FAILED,
} scli_state_t;

static const char *tag = "NimBLE_SCLI";
static TaskHandle_t cli_task;
static QueueHandle_t cli_handle;
static QueueHandle_t uart_queue;
static SemaphoreHandle_t cli_stopped;
static atomic_bool stop_requested = ATOMIC_VAR_INIT(false);
static bool uart_installed;
static bool console_initialized;
static scli_state_t cli_state = SCLI_STATE_STOPPED;
static portMUX_TYPE cli_state_lock = portMUX_INITIALIZER_UNLOCKED;

static void set_cli_state(scli_state_t state)
{
    taskENTER_CRITICAL(&cli_state_lock);
    cli_state = state;
    taskEXIT_CRITICAL(&cli_state_lock);
}

static int enter_passkey_handler(int argc, char *argv[])
{
    int key = 0;
    char pkey[8];

    if (argc != 2 || argv == NULL || argv[1] == NULL || cli_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (sscanf(argv[1], "%7s", pkey) != 1) {
        return ESP_ERR_INVALID_ARG;
    }

    if (isalpha((unsigned char)pkey[0])) {
        key = (strcasecmp(pkey, "Y") == 0 || strcasecmp(pkey, "Yes") == 0) ? 1 : 0;
    } else if (sscanf(pkey, "%d", &key) != 1) {
        return ESP_ERR_INVALID_ARG;
    }

    return xQueueOverwrite(cli_handle, &key) == pdPASS ? ESP_OK : ESP_FAIL;
}

int scli_receive_key(int *console_key)
{
    if (console_key == NULL || cli_handle == NULL) {
        return pdFALSE;
    }
    return xQueueReceive(cli_handle, console_key, pdMS_TO_TICKS(BLE_RX_TIMEOUT_MS));
}

static const esp_console_cmd_t cmds[] = {
    {
        .command = "key",
        .help    = "Provide a BLE pairing key or Y/N confirmation",
        .func    = enter_passkey_handler,
    },
};

static esp_err_t ble_register_cli(void)
{
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); ++i) {
        const esp_err_t rc = esp_console_cmd_register(&cmds[i]);
        if (rc != ESP_OK) {
            return rc;
        }
    }
    return ESP_OK;
}

static void scli_task(void *arg)
{
    const uart_port_t uart_num = (uart_port_t)(intptr_t)arg;
    uint8_t linebuf[256] = {0};
    size_t line_len = 0;

    while (!atomic_load_explicit(&stop_requested, memory_order_acquire)) {
        uart_event_t event;
        if (xQueueReceive(uart_queue, &event, pdMS_TO_TICKS(CLI_EVENT_POLL_MS)) != pdPASS) {
            continue;
        }

        if (event.type == UART_FIFO_OVF || event.type == UART_BUFFER_FULL) {
            ESP_LOGW(tag, "UART input overflow; dropping the partial CLI command");
            uart_flush_input(uart_num);
            xQueueReset(uart_queue);
            line_len = 0;
            continue;
        }
        if (event.type != UART_DATA) {
            continue;
        }

        uint8_t byte;
        while (!atomic_load_explicit(&stop_requested, memory_order_acquire) &&
               uart_read_bytes(uart_num, &byte, 1, 0) == 1) {
            if (byte == '\r' || byte == '\n') {
                if (line_len == 0) {
                    continue;
                }

                linebuf[line_len] = '\0';
                uart_write_bytes(uart_num, "\r\n", 2);

                int command_result = 0;
                const esp_err_t rc = esp_console_run((const char *)linebuf, &command_result);
                if (rc != ESP_OK) {
                    ESP_LOGW(tag, "CLI command failed: %s", esp_err_to_name(rc));
                } else if (command_result != ESP_OK) {
                    ESP_LOGW(tag, "CLI command returned: %d", command_result);
                }

                line_len = 0;
                memset(linebuf, 0, sizeof(linebuf));
                continue;
            }

            if (line_len >= sizeof(linebuf) - 1) {
                ESP_LOGW(tag, "CLI command is too long; dropping it");
                line_len = 0;
                memset(linebuf, 0, sizeof(linebuf));
                continue;
            }

            linebuf[line_len++] = byte;
            uart_write_bytes(uart_num, (const char *)&byte, 1);
        }
    }

    if (cli_stopped != NULL) {
        xSemaphoreGive(cli_stopped);
    }

    /*
     * Keep the task handle valid until scli_deinit() owns and deletes it.
     * This avoids polling a stale handle after a self-delete.
     */
    vTaskSuspend(NULL);
    vTaskDelete(NULL);
}

static esp_err_t scli_cleanup_resources(void)
{
    esp_err_t first_error = ESP_OK;

    if (console_initialized) {
        const esp_err_t rc = esp_console_deinit();
        if (rc != ESP_OK && first_error == ESP_OK) {
            first_error = rc;
        }
        console_initialized = false;
    }

    if (uart_installed) {
        const esp_err_t rc = uart_driver_delete(CLI_UART_NUM);
        if (rc != ESP_OK && first_error == ESP_OK) {
            first_error = rc;
        }
        uart_installed = false;
        uart_queue = NULL;
    }

    if (cli_handle != NULL) {
        vQueueDelete(cli_handle);
        cli_handle = NULL;
    }
    if (cli_stopped != NULL) {
        vSemaphoreDelete(cli_stopped);
        cli_stopped = NULL;
    }

    return first_error;
}

static esp_err_t scli_abort_init(esp_err_t cause)
{
    const esp_err_t cleanup_result = scli_cleanup_resources();
    set_cli_state(cleanup_result == ESP_OK ? SCLI_STATE_STOPPED : SCLI_STATE_FAILED);
    return cleanup_result == ESP_OK ? cause : cleanup_result;
}

int scli_init(void)
{
    taskENTER_CRITICAL(&cli_state_lock);
    if (cli_state == SCLI_STATE_RUNNING) {
        taskEXIT_CRITICAL(&cli_state_lock);
        return ESP_OK;
    }
    if (cli_state != SCLI_STATE_STOPPED) {
        taskEXIT_CRITICAL(&cli_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    cli_state = SCLI_STATE_STARTING;
    taskEXIT_CRITICAL(&cli_state_lock);

    atomic_store_explicit(&stop_requested, false, memory_order_release);
    cli_handle = xQueueCreate(1, sizeof(int));
    cli_stopped = xSemaphoreCreateBinary();
    if (cli_handle == NULL || cli_stopped == NULL) {
        return scli_abort_init(ESP_ERR_NO_MEM);
    }

    esp_err_t rc = uart_driver_install(CLI_UART_NUM, 256, 0, 8, &uart_queue, 0);
    if (rc != ESP_OK) {
        return scli_abort_init(rc);
    }
    uart_installed = true;

    const esp_console_config_t console_config = {
        .max_cmdline_args   = 8,
        .max_cmdline_length = 256,
    };
    rc = esp_console_init(&console_config);
    if (rc != ESP_OK) {
        return scli_abort_init(rc);
    }
    console_initialized = true;

    rc = ble_register_cli();
    if (rc != ESP_OK) {
        return scli_abort_init(rc);
    }

    if (xTaskCreate(scli_task, "scli_cli", 4096, (void *)(intptr_t)CLI_UART_NUM, 3, &cli_task) != pdPASS) {
        cli_task = NULL;
        return scli_abort_init(ESP_ERR_NO_MEM);
    }

    set_cli_state(SCLI_STATE_RUNNING);
    return ESP_OK;
}

int scli_deinit(void)
{
    taskENTER_CRITICAL(&cli_state_lock);
    if (cli_state == SCLI_STATE_STOPPED) {
        taskEXIT_CRITICAL(&cli_state_lock);
        return scli_cleanup_resources();
    }
    if (cli_state != SCLI_STATE_RUNNING) {
        taskEXIT_CRITICAL(&cli_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    cli_state = SCLI_STATE_STOPPING;
    taskEXIT_CRITICAL(&cli_state_lock);

    esp_err_t result = ESP_OK;
    atomic_store_explicit(&stop_requested, true, memory_order_release);

    if (cli_task != NULL) {
        if (cli_stopped == NULL ||
            xSemaphoreTake(cli_stopped, pdMS_TO_TICKS(CLI_STOP_TIMEOUT_MS)) != pdPASS) {
            ESP_LOGW(tag, "CLI task did not stop in time; forcing task deletion");
            result = ESP_ERR_TIMEOUT;
        }
        vTaskDelete(cli_task);
        cli_task = NULL;
    }

    const esp_err_t cleanup_result = scli_cleanup_resources();
    if (result == ESP_OK) {
        result = cleanup_result;
    }

    atomic_store_explicit(&stop_requested, false, memory_order_release);
    set_cli_state(cleanup_result == ESP_OK ? SCLI_STATE_STOPPED : SCLI_STATE_FAILED);
    return result;
}
