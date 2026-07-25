/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"
#include "board/hal_bridge.h"
#include "drivers/PY32IOExpander_Class/PY32IOExpander_Class.hpp"
#include <mooncake_log.h>
#include <array>
#include <memory>
#include <mutex>

static const std::string_view _tag = "HAL-IOE";

static std::unique_ptr<m5::PY32IOExpander_Class> _io_expander;
static std::array<uint8_t, 12 * 2> _rgb_buffer{};
static std::mutex _rgb_mutex;
static bool _rgb_dirty = false;
static uint32_t _rgb_last_error_log_ms = 0;
static uint32_t _rgb_next_retry_ms = 0;

void Hal::initIoExpander()
{
    mclog::tagInfo(_tag, "init");

    const bool servo_power_enabled = _registry.isAvailable(DeviceCapability::ServoPower);
    const bool rgb_enabled         = _registry.isAvailable(DeviceCapability::RgbLed);

    // Skip if force_disabled by config
    if (!servo_power_enabled && !rgb_enabled) {
        // Both were already pre-registered Unavailable by force_disabled
        mclog::tagInfo(_tag, "IO Expander skipped (both ServoPower and RgbLed force-disabled)");
        return;
    }

    auto i2c_bus          = hal_bridge::board_get_i2c_bus();
    _io_expander          = std::make_unique<m5::PY32IOExpander_Class>(i2c_bus);
    uint32_t start_tick   = GetHAL().millis();
    uint32_t retry_delay  = 50;
    constexpr uint32_t kInitTimeoutMs = 1200;

    if (!_io_expander->isRegistered()) {
        mclog::tagError(_tag, "failed to register IO expander on I2C bus");
        _io_expander.reset();
    } else {
        // Try immediately, then back off while a slowly booting expander becomes ready.
        while (!_io_expander->begin()) {
            uint32_t elapsed = GetHAL().millis() - start_tick;
            if (elapsed >= kInitTimeoutMs) {
                break;
            }

            uint32_t remaining = kInitTimeoutMs - elapsed;
            uint32_t wait_ms   = retry_delay < remaining ? retry_delay : remaining;
            mclog::tagInfo(_tag, "init failed, retrying in {} ms", wait_ms);
            vTaskDelay(pdMS_TO_TICKS(wait_ms));
            retry_delay = retry_delay < 400 ? retry_delay * 2 : 400;
        }

        if (!_io_expander->isInitialized()) {
            mclog::tagError(_tag, "init timeout");
            _io_expander.reset();
        }
    }

    if (_io_expander) {
        if (servo_power_enabled) {
            // VM EN
            _io_expander->setDirection(0, true);  // Output
            _io_expander->setPullMode(0, true);   // Pull-up
            // Power rail is turned on by initPowerRails() – not here
        }

        if (rgb_enabled) {
            // RGB
            _io_expander->setDirection(13, true);   // Output
            _io_expander->setPullMode(13, true);    // Pull-up
            _io_expander->setDriveMode(13, false);  // Push-pull
            _io_expander->setLedCount(12);
            GetHAL().showRgbColor(0, 0, 0);
            vTaskDelay(pdMS_TO_TICKS(10));
            GetHAL().showRgbColor(0, 0, 0);
        }

        mclog::tagInfo(_tag, "init done");
    } else {
        // IO Expander absent: servo power control and RGB unavailable
        if (servo_power_enabled) {
            _registry.set(DeviceCapability::ServoPower, false);
            _registry.setUnavailableReason(DeviceCapability::ServoPower, "IO expander not found");
        }
        if (rgb_enabled) {
            _registry.set(DeviceCapability::RgbLed, false);
            _registry.setUnavailableReason(DeviceCapability::RgbLed, "IO expander not found");
        }
        mclog::tagWarn(_tag, "IO Expander not found – ServoPower and RgbLed unavailable");
    }
}

void Hal::setServoPowerEnabled(bool enabled)
{
    if (!_io_expander || !_registry.isAvailable(DeviceCapability::ServoPower)) {
        return;
    }
    _io_expander->digitalWrite(0, enabled ? true : false);
}

void Hal::setRgbColor(uint8_t index, uint8_t r, uint8_t g, uint8_t b)
{
    if (!_io_expander || !_registry.isAvailable(DeviceCapability::RgbLed) || index >= 12) {
        return;
    }

    uint16_t color = static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    std::lock_guard<std::mutex> lock(_rgb_mutex);
    _rgb_buffer[index * 2]     = static_cast<uint8_t>(color & 0xFF);
    _rgb_buffer[index * 2 + 1] = static_cast<uint8_t>(color >> 8);
    _rgb_dirty                 = true;
}

void Hal::refreshRgb()
{
    if (!_io_expander || !_registry.isAvailable(DeviceCapability::RgbLed)) {
        return;
    }

    std::lock_guard<std::mutex> lock(_rgb_mutex);
    if (!_rgb_dirty) {
        return;
    }

    uint32_t now = GetHAL().millis();
    if (_rgb_next_retry_ms != 0 && static_cast<int32_t>(now - _rgb_next_retry_ms) < 0) {
        return;
    }

    esp_err_t err = _io_expander->setLedData(_rgb_buffer.data(), _rgb_buffer.size());
    if (err == ESP_OK) {
        err = _io_expander->refreshLeds();
    }
    if (err != ESP_OK) {
        if (_rgb_last_error_log_ms == 0 || now - _rgb_last_error_log_ms >= 1000) {
            mclog::tagWarn(_tag, "RGB update failed: {}", esp_err_to_name(err));
            _rgb_last_error_log_ms = now;
        }
        _rgb_next_retry_ms = now + 1000;
        return;
    }
    _rgb_next_retry_ms = 0;
    _rgb_dirty          = false;
}

void Hal::showRgbColor(uint8_t r, uint8_t g, uint8_t b)
{
    for (int i = 0; i < 12; i++) {
        setRgbColor(i, r, g, b);
    }
    refreshRgb();
}
