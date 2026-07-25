/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"
#include "board/hal_bridge.h"
#include "drivers/bmi270/bmi270.h"
#include "utils/motion_detector/motion_detector.h"
#include <mooncake_log.h>
#include <memory>

static const std::string_view _tag = "HAL-IMU";

static std::unique_ptr<BMI270> _bmi270;

static void _imu_task(void* param)
{
    auto motion_detector = std::make_unique<MotionDetector>();
    motion_detector->setShakeThreshold(16.0f);
    uint32_t consecutive_failures = 0;

    while (1) {
        if (_bmi270 && _bmi270->update()) {
            consecutive_failures = 0;
            auto& data = _bmi270->getData();

            motion_detector->update(data.accel_x, data.accel_y, data.accel_z);

            if (motion_detector->isShakeDetected()) {
                mclog::tagInfo(_tag, "Shake Detected!");
                GetHAL().onImuMotionEvent.emit(ImuMotionEvent::Shake);
            }
        } else {
            ++consecutive_failures;
        }

        uint32_t delay_ms = 100;
        if (consecutive_failures > 0) {
            uint32_t shift = consecutive_failures > 3 ? 3 : consecutive_failures;
            delay_ms       = 100U << shift;
        }
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

// ---------------------------------------------------------------------------
// Hal::imu_init_impl – returns true if BMI270 was found and initialised
// ---------------------------------------------------------------------------
bool Hal::imu_init_impl()
{
    auto i2c_bus = hal_bridge::board_get_i2c_bus();
    _bmi270      = std::make_unique<BMI270>(i2c_bus, 0x69);

    if (!_bmi270->begin()) {
        _bmi270.reset();
        mclog::tagWarn(_tag, "BMI270 init failed");
        return false;
    }

    mclog::tagInfo(_tag, "BMI270 init ok");
    BaseType_t task_created =
        xTaskCreatePinnedToCoreWithCaps(_imu_task, "imu", 4096, NULL, 2, NULL, 1, MALLOC_CAP_SPIRAM);
    if (task_created != pdPASS) {
        mclog::tagWarn(_tag, "failed to create BMI270 update task");
        _bmi270.reset();
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Hal::initI2cSensors – dispatcher for IMU / RTC / HeadTouch (§6.6-6.5)
// ---------------------------------------------------------------------------
void Hal::initI2cSensors()
{
    mclog::tagInfo(_tag, "initI2cSensors");

    // ── IMU ──────────────────────────────────────────────────────────────────
    if (_registry.isAvailable(DeviceCapability::Imu)) {
        bool ok = imu_init_impl();
        if (!ok) {
            _registry.set(DeviceCapability::Imu, false);
            _registry.setUnavailableReason(DeviceCapability::Imu, "BMI270 begin failed");
        }
        // If ok, leave as Available (already set during force_disabled pass)
        // But we need to explicitly set it Available if it wasn't pre-set:
        if (ok) {
            _registry.set(DeviceCapability::Imu, true);
        }
    } else {
        mclog::tagInfo(_tag, "IMU skipped (force-disabled)");
    }

    // ── RTC ──────────────────────────────────────────────────────────────────
    if (_registry.isAvailable(DeviceCapability::Rtc)) {
        bool ok = rtc_init_impl();
        if (ok) {
            _registry.set(DeviceCapability::Rtc, true);
        } else {
            _registry.set(DeviceCapability::Rtc, false);
            _registry.setUnavailableReason(DeviceCapability::Rtc, "PCF8563 begin failed");
        }
    } else {
        mclog::tagInfo(_tag, "RTC skipped (force-disabled)");
    }

    // ── Head Touch ───────────────────────────────────────────────────────────
    if (_registry.isAvailable(DeviceCapability::HeadTouch)) {
        bool ok = head_touch_init_impl();
        if (ok) {
            _registry.set(DeviceCapability::HeadTouch, true);
        } else {
            _registry.set(DeviceCapability::HeadTouch, false);
            _registry.setUnavailableReason(DeviceCapability::HeadTouch, "SI12T init failed");
        }
    } else {
        mclog::tagInfo(_tag, "HeadTouch skipped (force-disabled)");
    }
}
