/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"
#include "board/hal_bridge.h"
#include "drivers/PY32IOExpander_Class/PY32IOExpander_Class.hpp"
#include <mooncake_log.h>
#include <memory>

static const std::string_view _tag = "HAL-IOE";

static std::unique_ptr<m5::PY32IOExpander_Class> _io_expander;

void Hal::initIoExpander()
{
    mclog::tagInfo(_tag, "init");

    // Skip if force_disabled by config
    if (!_registry.isAvailable(DeviceCapability::ServoPower) &&
        !_registry.isAvailable(DeviceCapability::RgbLed)) {
        // Both were already pre-registered Unavailable by force_disabled
        mclog::tagInfo(_tag, "IO Expander skipped (both ServoPower and RgbLed force-disabled)");
        return;
    }

    auto i2c_bus        = hal_bridge::board_get_i2c_bus();
    _io_expander        = std::make_unique<m5::PY32IOExpander_Class>(i2c_bus);
    uint32_t start_tick = GetHAL().millis();

    // PY32 IO Expander may boot slowly, wait for it
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(200));

        if (GetHAL().millis() - start_tick > 1200) {
            mclog::tagError(_tag, "init timeout");
            _io_expander.reset();
            break;
        }

        if (_io_expander->begin()) {
            break;
        }
        mclog::tagInfo(_tag, "init failed, retrying...");
    }

    if (_io_expander) {
        // VM EN
        _io_expander->setDirection(0, true);  // Output
        _io_expander->setPullMode(0, true);   // Pull-up
        // Power rail is turned on by initPowerRails() – not here

        // RGB
        _io_expander->setDirection(13, true);   // Output
        _io_expander->setPullMode(13, true);    // Pull-up
        _io_expander->setDriveMode(13, false);  // Push-pull
        _io_expander->setLedCount(12);
        vTaskDelay(pdMS_TO_TICKS(200));
        GetHAL().showRgbColor(0, 0, 0);
        vTaskDelay(pdMS_TO_TICKS(50));
        GetHAL().showRgbColor(0, 0, 0);

        // Register capabilities
        _registry.set(DeviceCapability::ServoPower, true);
        _registry.set(DeviceCapability::RgbLed,     true);

        mclog::tagInfo(_tag, "init done");
    } else {
        // IO Expander absent: servo power control and RGB unavailable
        _registry.set(DeviceCapability::ServoPower, false);
        _registry.set(DeviceCapability::RgbLed,     false);
        _registry.setUnavailableReason(DeviceCapability::ServoPower, "IO expander not found");
        _registry.setUnavailableReason(DeviceCapability::RgbLed,     "IO expander not found");
        mclog::tagWarn(_tag, "IO Expander not found – ServoPower and RgbLed unavailable");
    }
}

void Hal::setServoPowerEnabled(bool enabled)
{
    if (!_io_expander) {
        return;
    }
    _io_expander->digitalWrite(0, enabled ? true : false);
}

void Hal::setRgbColor(uint8_t index, uint8_t r, uint8_t g, uint8_t b)
{
    if (!_io_expander) {
        return;
    }
    _io_expander->setLedColor(index, r, g, b);
}

void Hal::refreshRgb()
{
    if (!_io_expander) {
        return;
    }
    _io_expander->refreshLeds();
}

void Hal::showRgbColor(uint8_t r, uint8_t g, uint8_t b)
{
    for (int i = 0; i < 12; i++) {
        setRgbColor(i, r, g, b);
    }
    refreshRgb();
}
