/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "device_capability.h"
#include <esp_log.h>

static const char* _tag = "StackChan";

// Helper: name string for each capability
static const char* cap_name(DeviceCapability cap)
{
    switch (cap) {
        case DeviceCapability::Display:    return "Display";
        case DeviceCapability::Microphone: return "Microphone";
        case DeviceCapability::Speaker:    return "Speaker";
        case DeviceCapability::Camera:     return "Camera";
        case DeviceCapability::ServoYaw:   return "ServoYaw";
        case DeviceCapability::ServoPitch: return "ServoPitch";
        case DeviceCapability::ServoPower: return "ServoPower";
        case DeviceCapability::RgbLed:     return "RgbLed";
        case DeviceCapability::HeadTouch:  return "HeadTouch";
        case DeviceCapability::Imu:        return "Imu";
        case DeviceCapability::Rtc:        return "Rtc";
        case DeviceCapability::IrTx:       return "IrTx";
        case DeviceCapability::IrRx:       return "IrRx";
        case DeviceCapability::Nfc:        return "Nfc";
        case DeviceCapability::EspNow:     return "EspNow";
        case DeviceCapability::Ble:        return "Ble";
        case DeviceCapability::Wifi:       return "Wifi";
        case DeviceCapability::Ota:        return "Ota";
        default:                           return "Unknown";
    }
}

void DeviceRegistry::printSummary() const
{
    ESP_LOGI(_tag, "=== Device Capability Summary ===");
    for (size_t i = 0; i < kCount; i++) {
        auto cap  = static_cast<DeviceCapability>(i);
        bool avail = _available.test(i);
        const char* name = cap_name(cap);

        if (avail) {
            ESP_LOGI(_tag, "  %-14s: Available", name);
        } else {
            if (!_reasons[i].empty()) {
                ESP_LOGW(_tag, "  %-14s: Unavailable (%.*s)",
                         name,
                         static_cast<int>(_reasons[i].size()),
                         _reasons[i].data());
            } else {
                ESP_LOGW(_tag, "  %-14s: Unavailable", name);
            }
        }
    }
    ESP_LOGI(_tag, "=================================");
}
