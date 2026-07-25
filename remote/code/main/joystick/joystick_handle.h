/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef __JOYSTICK_HANDLE_H__
#define __JOYSTICK_HANDLE_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "driver/i2c.h"
#include "i2c_bus.h"
#include "hal/i2c_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "../esp_now/esp_now_init.h"
#include "../ui/ui_setup_screen.h"
#include "../ui/ui_running_screen.h"
#include "../ui/ui_imu_screen.h"
#include "joystick_basic.h"

joystick_data_t joystick_init();
bool joystick_task_gate_init(void);
void joystick_task_gate_open(void);
void joystick_task_gate_deinit(void);
void joystick_data_lock(void);
void joystick_data_unlock(void);
void joystick_notify_mode_change(uint8_t screen_mode);
void handle_setup_screen(void *pvParam);
void handle_running_screen(void *pvParam);
void handle_imu_screen(void *pvParam);

#ifdef __cplusplus
}
#endif

#endif
