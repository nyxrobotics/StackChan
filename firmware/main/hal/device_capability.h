/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <bitset>
#include <cstdint>
#include <string_view>

// ─────────────────────────────────────────────────────────────
// DeviceCapability
// ─────────────────────────────────────────────────────────────

/**
 * @brief Enumeration of all optional / required device capabilities.
 *
 * Each entry maps to one bit in DeviceRegistry.
 * _Count must always be last – it is used as the bitset size.
 */
enum class DeviceCapability : uint8_t {
    Display,
    Microphone,
    Speaker,
    Camera,
    ServoYaw,
    ServoPitch,
    ServoPower,
    RgbLed,
    HeadTouch,
    Imu,
    Rtc,
    IrTx,
    IrRx,
    Nfc,
    EspNow,
    Ble,
    Wifi,
    Ota,
    _Count
};

// ─────────────────────────────────────────────────────────────
// DeviceRegistry
// ─────────────────────────────────────────────────────────────

/**
 * @brief Tracks Available / Unavailable state for every DeviceCapability.
 *
 * Design rules:
 *  - Written only during HAL initialisation (boot phase).
 *  - After lock() is called no further writes are allowed.
 *  - isAvailable() is safe to call from any context after boot.
 *  - HAL / StackChan internal code may call isAvailable().
 *    Application code should NOT call it directly.
 */
class DeviceRegistry {
public:
    /**
     * @brief Register the availability of a capability.
     *
     * Silently ignored after lock() has been called to preserve
     * write-once semantics.
     *
     * @param cap       The capability to update.
     * @param available true = Available, false = Unavailable.
     */
    void set(DeviceCapability cap, bool available)
    {
        if (_locked) return;
        _available.set(static_cast<size_t>(cap), available);
    }

    /**
     * @brief Query whether a capability is Available.
     */
    bool isAvailable(DeviceCapability cap) const
    {
        return _available.test(static_cast<size_t>(cap));
    }

    /**
     * @brief Lock the registry against further writes.
     *
     * Called once, after all device initialisation is complete.
     */
    void lock()
    {
        _locked = true;
    }

    /**
     * @brief Print a human-readable capability summary to the log.
     *
     * Format matches §9.3 of the spec:
     *   I [StackChan]   ServoYaw     : Available
     *   W [StackChan]   ServoPitch   : Unavailable (...)
     */
    void printSummary() const;

    // Allow HAL to attach a reason string before lock() for Unavailable entries.
    void setUnavailableReason(DeviceCapability cap, std::string_view reason)
    {
        if (_locked) return;
        size_t idx = static_cast<size_t>(cap);
        if (idx < static_cast<size_t>(DeviceCapability::_Count)) {
            _reasons[idx] = reason;
        }
    }

private:
    static constexpr size_t kCount = static_cast<size_t>(DeviceCapability::_Count);

    std::bitset<kCount> _available;
    bool _locked = false;

    // Optional Unavailable reason strings (set during init, printed in summary).
    // Using a fixed-size array of string_view pointing to string literals is safe
    // because the literals live for the duration of the program.
    std::string_view _reasons[kCount] = {};
};
