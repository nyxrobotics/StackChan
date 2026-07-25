/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "M5Unified.h"

extern "C" {
#include <stdio.h>
#include "esp_log.h"
#include "string.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "lvgl.h"

#include "ui.h"
#include "esp_now_init.h"
#include "joystick_handle.h"

#include "lvgl_port.h"

using namespace m5;

joystick_data_t joystick_data;

// extern void lvgl_port_init(M5GFX &gfx);

/**
 * @brief Handle Button Press.
 * 1. Press BtnA to switch setup_mode UI and running_mode UI.
 * 2. Press BtnB to switch espnow-channel or id on setup_mode;
 * 3. Press BtnB to send btnB_status to remote on running_mode.
 */
void handle_button_press()
{
    static uint8_t screen_mode = MODE_SETUP;
    // check if BtnA is pressed
    if (M5.BtnA.wasPressed()) {
        // use BtnA to switch mode
        uint8_t next_screen_mode = (screen_mode + 1) % 3;
        if (next_screen_mode == MODE_RUNNING) {
            int channel;
            joystick_data_lock();
            channel = joystick_data.channel;
            joystick_data_unlock();
            if (wifi_espnow_set_channel((uint8_t)channel) < 0) {
                ESP_LOGE("APP", "Failed to change ESP-NOW channel");
                return;
            }
        }

        // Publish the mode only after all target-screen objects are ready and loaded.
        if (switch_screen(next_screen_mode)) {
            screen_mode = next_screen_mode;
            joystick_data_lock();
            joystick_data.screen_mode = next_screen_mode;
            joystick_data_unlock();
            joystick_notify_mode_change(next_screen_mode);
        } else {
            ESP_LOGE("APP", "Failed to switch screen mode to %u", next_screen_mode);
        }
    }
    if (M5.BtnB.wasPressed()) {
        joystick_data_lock();
        if (joystick_data.screen_mode == MODE_SETUP) {
            joystick_data.select_mode = !joystick_data.select_mode;
        } else if ((joystick_data.screen_mode == MODE_RUNNING) || (joystick_data.screen_mode == MODE_IMU)) {
            joystick_data.btnB_status = !joystick_data.btnB_status;
        }
        joystick_data_unlock();
    }
}

void app_main(void)
{
    imu_data_t imu_data;

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ret = nvs_flash_erase();
        if (ret != ESP_OK) {
            ESP_LOGE("APP", "Failed to erase NVS: %s", esp_err_to_name(ret));
            return;
        }
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE("APP", "Failed to initialize NVS: %s", esp_err_to_name(ret));
        return;
    }

    M5.begin();
    M5.Power.begin();
    M5.Lcd.setBrightness(100);  // set brightness to 100
    M5.Imu.init(&M5.In_I2C);    // init IMU with internal I2C port
    printf("IN_I2C port: %d\n", M5.In_I2C.getPort());
    printf("M5 Display width: %ld, height: %ld\n", M5.Display.width(), M5.Display.height());

    joystick_data = joystick_init();  // init joystick

    if (!lvgl_port_init()) {  // init LVGL
        ESP_LOGE("APP", "LVGL initialization failed");
        return;
    }
    if (!ui_init()) {  // init UI
        ESP_LOGE("APP", "UI initialization failed");
        return;
    }

    // init WiFi and ESP-NOW
    ret = wifi_espnow_init(joystick_data.channel);
    if (ret != ESP_OK) {
        ESP_LOGE("APP", "Failed to initialize ESP-NOW: %s", esp_err_to_name(ret));
        return;
    }

    TaskHandle_t setup_task = NULL;
    TaskHandle_t running_task = NULL;
    TaskHandle_t imu_task = NULL;
    // Every task blocks on this shared gate before touching LVGL or shared
    // joystick state. This also makes partial-creation rollback safe on SMP.
    if (!joystick_task_gate_init()) {
        ESP_LOGE("APP", "Failed to create joystick task start gate");
        return;
    }
    BaseType_t setup_created =
        xTaskCreate(handle_setup_screen, "handle_setup_screen", 8192, &joystick_data, 5, &setup_task);
    BaseType_t running_created =
        setup_created == pdPASS
            ? xTaskCreate(handle_running_screen, "handle_running_screen", 8192, &joystick_data, 5, &running_task)
            : pdFAIL;
    BaseType_t imu_created =
        running_created == pdPASS ? xTaskCreate(handle_imu_screen, "handle_imu_screen", 8192, &joystick_data, 5, &imu_task)
                                  : pdFAIL;
    const bool tasks_created = setup_created == pdPASS && running_created == pdPASS && imu_created == pdPASS;
    if (!tasks_created) {
        ESP_LOGE("APP", "Failed to create joystick task");
        if (setup_task != NULL) vTaskDelete(setup_task);
        if (running_task != NULL) vTaskDelete(running_task);
        if (imu_task != NULL) vTaskDelete(imu_task);
        joystick_task_gate_deinit();
        return;
    }
    joystick_task_gate_open();

    while (1) {
        M5.update();
        // Handle button press
        handle_button_press();
        int battery_level = M5.Power.Axp192.getBatteryLevel();
        battery_level     = (battery_level > 100) ? 100 : battery_level;
        battery_level     = (battery_level < 0) ? 0 : battery_level;

        M5.Imu.update();                              // update IMU data
        imu_data              = M5.Imu.getImuData();  // get IMU data
        joystick_data_lock();
        joystick_data.bat     = (int8_t)battery_level;
        joystick_data.accel_x = imu_data.accel.x;
        joystick_data.accel_y = imu_data.accel.y;
        joystick_data.accel_z = imu_data.accel.z;
        joystick_data_unlock();

#if 0
        printf("Accel: (%.2f, %.2f, %.2f), Gyro: (%.2f, %.2f, %.2f)\n",
               joystick_data.accel_x, joystick_data.accel_y, joystick_data.accel_z,
               joystick_data.gyro_x, joystick_data.gyro_y, joystick_data.gyro_z);
#endif
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
}
