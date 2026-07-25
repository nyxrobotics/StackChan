/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "lvgl_port.h"
#include <M5Unified.h>
#include "M5GFX.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

extern "C" {

#define LV_BUFFER_LINE 40
static SemaphoreHandle_t xGuiSemaphore = NULL;
static esp_timer_handle_t lvgl_tick_timer_handle = NULL;
static TaskHandle_t lvgl_task_handle = NULL;
static lv_color_t *lvgl_draw_buffer = NULL;
static lv_disp_t *lvgl_display = NULL;
static lv_indev_t *lvgl_input_device = NULL;
static bool lvgl_port_initialized = false;
static void lvgl_tick_timer(void *arg)
{
    (void)arg;
    lv_tick_inc(10);
}

static void lvgl_rtos_task(void *pvParameter)
{
    (void)pvParameter;
    while (1) {
        if (xGuiSemaphore != NULL && pdTRUE == xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(1000))) {
            lv_timer_handler();
            xSemaphoreGive(xGuiSemaphore);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static lv_disp_draw_buf_t draw_buf;

static void lvgl_release_registered_drivers(void)
{
    if (lvgl_input_device != NULL) {
        lv_indev_delete(lvgl_input_device);
        lvgl_input_device = NULL;
    }
    if (lvgl_display != NULL) {
        lv_disp_remove(lvgl_display);
        lvgl_display = NULL;
    }
    if (lvgl_draw_buffer != NULL) {
        heap_caps_free(lvgl_draw_buffer);
        lvgl_draw_buffer = NULL;
    }
}

static void lvgl_flush_cb(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p)
{
    int w           = (area->x2 - area->x1 + 1);
    int h           = (area->y2 - area->y1 + 1);
    uint32_t pixels = w * h;

    M5.Display.startWrite();
    M5.Display.setAddrWindow(area->x1, area->y1, w, h);

    // Critical fix: Use safe pixel writing method to avoid M5GFX SIMD optimizations
    // Break large transfers into small chunks to avoid problematic copy_rgb_fast function
    const uint32_t SAFE_CHUNK_SIZE = 8192;  // 8K pixels per chunk, suitable for small buffer settings

    if (pixels > SAFE_CHUNK_SIZE) {
        // Chunked transmission for large data
        const lgfx::rgb565_t *src = (const lgfx::rgb565_t *)color_p;
        uint32_t remaining        = pixels;
        uint32_t offset           = 0;

        while (remaining > 0) {
            uint32_t chunk_size = (remaining > SAFE_CHUNK_SIZE) ? SAFE_CHUNK_SIZE : remaining;
            M5.Display.writePixels(src + offset, chunk_size);
            offset += chunk_size;
            remaining -= chunk_size;
        }
    } else {
        // Direct transmission for small data
        M5.Display.writePixels((lgfx::rgb565_t *)color_p, pixels);
    }

    M5.Display.endWrite();

    lv_disp_flush_ready(disp);
}

// BtnA / BtnB
static void lvgl_read_cb(lv_indev_drv_t *indev_driver, lv_indev_data_t *data)
{
    (void)indev_driver;
    data->state  = LV_INDEV_STATE_REL;
    data->btn_id = 0;
}

bool lvgl_port_init(void)
{
    if (lvgl_port_initialized) {
        return true;
    }

    xGuiSemaphore = xSemaphoreCreateMutex();
    if (xGuiSemaphore == NULL) {
        ESP_LOGE("LVGL", "Failed to create GUI mutex!");
        return false;
    }

    lv_init();

    size_t buffer_size = M5.Display.width() * LV_BUFFER_LINE * sizeof(lv_color_t);
    lvgl_draw_buffer =
        (lv_color_t *)heap_caps_malloc(buffer_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);

    if (lvgl_draw_buffer == NULL) {
        ESP_LOGE("LVGL", "Failed to allocate display buffer!");
        vSemaphoreDelete(xGuiSemaphore);
        xGuiSemaphore = NULL;
        return false;
    }

    lv_disp_draw_buf_init(&draw_buf, lvgl_draw_buffer, NULL, M5.Display.width() * LV_BUFFER_LINE);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res   = M5.Display.width();
    disp_drv.ver_res   = M5.Display.height();
    disp_drv.flush_cb  = lvgl_flush_cb;
    disp_drv.draw_buf  = &draw_buf;
    disp_drv.user_data = &M5.Display;
    lvgl_display = lv_disp_drv_register(&disp_drv);
    if (lvgl_display == NULL) {
        ESP_LOGE("LVGL", "Failed to register display driver");
        lvgl_release_registered_drivers();
        vSemaphoreDelete(xGuiSemaphore);
        xGuiSemaphore = NULL;
        return false;
    }

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type      = LV_INDEV_TYPE_BUTTON;
    indev_drv.read_cb   = lvgl_read_cb;
    indev_drv.user_data = &M5.Display;
    lvgl_input_device = lv_indev_drv_register(&indev_drv);
    if (lvgl_input_device == NULL) {
        ESP_LOGE("LVGL", "Failed to register input driver");
        lvgl_release_registered_drivers();
        vSemaphoreDelete(xGuiSemaphore);
        xGuiSemaphore = NULL;
        return false;
    }

    esp_timer_create_args_t periodic_timer_args = {};
    periodic_timer_args.callback                = &lvgl_tick_timer;
    periodic_timer_args.name                    = "lvgl_tick_timer";
    esp_err_t ret = esp_timer_create(&periodic_timer_args, &lvgl_tick_timer_handle);
    if (ret != ESP_OK) {
        ESP_LOGE("LVGL", "Failed to create tick timer: %s", esp_err_to_name(ret));
        lvgl_release_registered_drivers();
        vSemaphoreDelete(xGuiSemaphore);
        xGuiSemaphore = NULL;
        return false;
    }
    ret = esp_timer_start_periodic(lvgl_tick_timer_handle, 10 * 1000);
    if (ret != ESP_OK) {
        ESP_LOGE("LVGL", "Failed to start tick timer: %s", esp_err_to_name(ret));
        esp_timer_delete(lvgl_tick_timer_handle);
        lvgl_tick_timer_handle = NULL;
        lvgl_release_registered_drivers();
        vSemaphoreDelete(xGuiSemaphore);
        xGuiSemaphore = NULL;
        return false;
    }
    if (xTaskCreate(lvgl_rtos_task, "lvgl_rtos_task", 4096, NULL, 1, &lvgl_task_handle) != pdPASS) {
        ESP_LOGE("LVGL", "Failed to create LVGL task");
        esp_timer_stop(lvgl_tick_timer_handle);
        esp_timer_delete(lvgl_tick_timer_handle);
        lvgl_tick_timer_handle = NULL;
        lvgl_release_registered_drivers();
        vSemaphoreDelete(xGuiSemaphore);
        xGuiSemaphore = NULL;
        return false;
    }
    lvgl_port_initialized = true;
    return true;
}

bool lvgl_port_lock(void)
{
    if (xGuiSemaphore == NULL) {
        return false;
    }
    return xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(1000)) == pdTRUE;
}

void lvgl_port_unlock(void)
{
    if (xGuiSemaphore != NULL) {
        xSemaphoreGive(xGuiSemaphore);
    }
}
}
