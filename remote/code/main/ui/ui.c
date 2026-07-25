/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "ui.h"
#include "../lvgl_port.h"

static bool load_screen_safely(lv_obj_t *screen, bool animated)
{
    if (!lvgl_port_lock()) {
        return false;
    }

    bool is_valid = screen != NULL && lv_obj_is_valid(screen);
    if (is_valid) {
        if (animated) {
            lv_scr_load_anim(screen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, false);
        } else {
            lv_disp_load_scr(screen);
        }
    }

    lvgl_port_unlock();
    return is_valid;
}

/**
 * @brief Switches between different UI screens based on the provided screen ID
 *
 * This function manages the display of different UI screens (setup, running, IMU)
 * by checking if the requested screen exists, creating it if necessary, validating
 * the screen object, and then loading it with a slide-left animation effect.
 *
 * The function implements bounded error recovery by destroying an invalid screen
 * and retrying its creation once.
 *
 * @param screen_id An integer representing the target screen mode:
 *                  - MODE_SETUP: Configuration/setup screen
 *                  - MODE_RUNNING: Main operational screen
 *                  - MODE_IMU: IMU data visualization screen
 *                  - Any other value: Logs an error message
 *
 * @note The function uses LVGL's animation API to provide smooth screen transitions
 *       with a 200ms left slide animation. All LVGL validation and loading is
 *       serialized by the LVGL port mutex.
 */
bool switch_screen(int screen_id)
{
    if (screen_id == MODE_SETUP) {
        for (int attempt = 0; attempt < 2; attempt++) {
            if (setup_screen == NULL) {
                create_setup_screen();
                ESP_LOGI("UI", "Setup screen created");
            }
            // Validate and load while holding the same mutex used by LVGL updates.
            if (load_screen_safely(setup_screen, true)) {
                ESP_LOGI("UI", "Setup screen loaded");
                return true;
            }
            ESP_LOGE("UI", "Setup screen is NULL or invalid!");
            ui_setup_screen_destory();
        }
        ESP_LOGE("UI", "Failed to recreate setup screen");
    } else if (screen_id == MODE_RUNNING) {
        for (int attempt = 0; attempt < 2; attempt++) {
            if (running_screen == NULL) {
                create_running_screen();
                ESP_LOGI("UI", "Running screen created");
            }
            // Validate and load while holding the same mutex used by LVGL updates.
            if (load_screen_safely(running_screen, true)) {
                ESP_LOGI("UI", "Running screen loaded");
                return true;
            }
            ESP_LOGE("UI", "Running screen is NULL or invalid!");
            ui_running_screen_destory();
        }
        ESP_LOGE("UI", "Failed to recreate running screen");
    } else if (screen_id == MODE_IMU) {
        for (int attempt = 0; attempt < 2; attempt++) {
            if (imu_screen == NULL) {
                create_imu_screen();
                ESP_LOGI("UI", "IMU screen created");
            }
            // Validate and load while holding the same mutex used by LVGL updates.
            if (load_screen_safely(imu_screen, true)) {
                ESP_LOGI("UI", "IMU screen loaded");
                return true;
            }
            ESP_LOGE("UI", "IMU screen is NULL or invalid!");
            ui_imu_screen_destory();
        }
        ESP_LOGE("UI", "Failed to recreate IMU screen");
    } else {
        ESP_LOGE("UI", "Invalid screen mode!");
    }
    return false;
}

/**
 * @brief Initialize the UI system by creating and loading the initial setup screen
 * @note This function serves as the entry point for UI initialization
 * @details
 *      1. Creates the setup screen using create_setup_screen()
 *      2. Immediately loads the setup screen as the current display
 *      3. Sets up the initial UI state for user interaction
 * @warning This function should only be called once during application startup
 */
bool ui_init()
{
    create_setup_screen();
    if (!load_screen_safely(setup_screen, false)) {
        ESP_LOGE("UI", "Failed to load setup screen!");
        return false;
    }
    return true;
}
