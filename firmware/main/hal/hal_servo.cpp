/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"
#include "drivers/FTServo_Arduino/src/SCSCL.h"
#include <stackchan/stackchan.h>
#include <smooth_ui_toolkit.hpp>
#include <mooncake_log.h>
#include <settings.h>
#include <driver/uart.h>
#include <driver/gpio.h>

using namespace smooth_ui_toolkit;
using namespace stackchan::motion;

// Servo UART hardware pins (must match the BSP / board wiring)
static constexpr uart_port_t SERVO_UART_NUM = UART_NUM_1;
static constexpr int         SERVO_TX_PIN   = 6;
static constexpr int         SERVO_RX_PIN   = 7;
static constexpr int         SERVO_BAUD     = 1000000;

// Default servo IDs
static constexpr int SERVO_YAW_ID   = 1;
static constexpr int SERVO_PITCH_ID = 2;

// ─────────────────────────────────────────────────────────────────────────────
// File-level SCS bus (shared between ScsServo instances and the probe phase)
// ─────────────────────────────────────────────────────────────────────────────
static SCSCL _scs_bus;

// ─────────────────────────────────────────────────────────────────────────────
// ScsServo – concrete Servo implementation driving one SCS axis
// ─────────────────────────────────────────────────────────────────────────────
struct ServoConfig_t {
    int id             = -1;
    int defaultZeroPos = 0;
    Vector2i angleLimit;
    Vector2i rawPosLimit;
    std::string settingNs;
    std::string settingZeroPositionKey;
    bool enablePwmMode = false;
    bool enableStallProtection = false;
};

class ScsServo : public Servo {
public:
    static inline const std::string _tag = "ScsServo";

    ScsServo(const ServoConfig_t& config) : _config(config), _runtime_raw_pos_limit(config.rawPosLimit)
    {
    }

    void init() override
    {
        reset_runtime_limits();
        set_angle_limit(_config.angleLimit);
        get_zero_pos_from_nvs();
        Servo::init();
    }

    void get_zero_pos_from_nvs()
    {
        _zero_pos     = _config.defaultZeroPos;
        bool is_valid = false;

        {
            Settings settings(_config.settingNs, false);
            int nvs_zero_pos = settings.GetInt(_config.settingZeroPositionKey, -1);

            if (nvs_zero_pos >= _config.rawPosLimit.x && nvs_zero_pos <= _config.rawPosLimit.y) {
                _zero_pos = nvs_zero_pos;
                is_valid  = true;
                mclog::tagInfo(_tag, "id: {} get zero pos: {} from settings", _config.id, _zero_pos);
            } else {
                mclog::tagWarn(_tag, "id: {} get invalid zero pos: {} from settings", _config.id, nvs_zero_pos);
            }
        }

        if (!is_valid) {
            _zero_pos = _config.defaultZeroPos;
            mclog::tagInfo(_tag, "id: {} override zero pos to default: {}", _config.id, _zero_pos);

            Settings settings(_config.settingNs, true);
            settings.SetInt(_config.settingZeroPositionKey, _zero_pos);
        }
    }

    void set_angle_impl(int angle) override
    {
        int mapped_angle = _zero_pos + angle * 16 / 5 / 10;  // 一步对应 0.3125度, 0.3125 = 5/16
        mapped_angle     = uitk::clamp(mapped_angle, _runtime_raw_pos_limit.x, _runtime_raw_pos_limit.y);

        if (update_stall_protection(mapped_angle)) {
            return;
        }

        check_mode(Mode::Position);
        _scs_bus.WritePos(_config.id, mapped_angle, 20, 0);
    }

    int getCurrentAngle() override
    {
        int current_pos = _scs_bus.ReadPos(_config.id);
        if (!is_raw_pos_valid(current_pos)) {
            int fallback_angle = uitk::clamp(Servo::getCurrentAngle(), getAngleLimit().x, getAngleLimit().y);
            mclog::tagWarn(_tag, "id: {} ignore invalid current pos: {}, fallback angle: {}", _config.id, current_pos,
                           fallback_angle);
            return fallback_angle;
        }

        int angle = raw_pos_to_angle(current_pos);
        angle     = uitk::clamp(angle, getAngleLimit().x, getAngleLimit().y);
        return angle;
    }

    bool is_moving_impl() override
    {
        return _scs_bus.ReadMove(_config.id) != 0;
    }

    void setTorqueEnabled(bool enabled) override
    {
        Servo::setTorqueEnabled(enabled);
        _scs_bus.EnableTorque(_config.id, enabled ? 1 : 0);
    }

    bool getTorqueEnabled() override
    {
        return _scs_bus.ReadToqueEnable(_config.id) > 0;
    }

    void setCurrentAngleAsZero() override
    {
        int current_pos = _scs_bus.ReadPos(_config.id);
        if (!is_raw_pos_valid(current_pos)) {
            mclog::tagWarn(_tag, "id: {} ignore invalid zero calibration pos: {}, keep zero pos: {}", _config.id,
                           current_pos, _zero_pos);
            return;
        }

        _zero_pos = current_pos;
        reset_runtime_limits();

        Settings settings(_config.settingNs, true);
        settings.SetInt(_config.settingZeroPositionKey, _zero_pos);
        mclog::tagInfo(_tag, "id: {} set zero pos: {}", _config.id, _zero_pos);
    }

    void resetZeroCalibration() override
    {
        _zero_pos = _config.defaultZeroPos;
        reset_runtime_limits();

        Settings settings(_config.settingNs, true);
        settings.SetInt(_config.settingZeroPositionKey, _zero_pos);
        mclog::tagInfo(_tag, "id: {} reset zero pos: {}", _config.id, _zero_pos);
    }

    void rotate(int velocity) override
    {
        if (!_config.enablePwmMode) return;
        velocity = uitk::clamp(velocity, -1000, 1000);
        int mapped_velocity = map_range(velocity, 0, 1000, 0, 1023);
        check_mode(Mode::PWM);
        _scs_bus.WritePWM(_config.id, mapped_velocity);
    }

private:
    enum class Mode { Position = 0, PWM = 1 };

    ServoConfig_t _config;
    Vector2i _runtime_raw_pos_limit;
    int _zero_pos      = 0;
    Mode _current_mode = Mode::Position;

    static constexpr uint32_t kStallFeedbackIntervalMs = 50;
    static constexpr int kStallMinTargetDeltaRaw       = 8;
    static constexpr int kStallMaxPositionDeltaRaw     = 1;
    static constexpr int kStallCurrentRiseThreshold    = 80;
    static constexpr int kStallLoadRiseThreshold       = 150;
    static constexpr int kStallCurrentAbsThreshold     = 350;
    static constexpr int kStallLoadAbsThreshold        = 650;
    static constexpr int kStallConfirmSamples          = 2;

    uint32_t _last_stall_check_tick = 0;
    int _last_stall_raw_pos         = 0;
    int _last_stall_current_abs     = 0;
    int _last_stall_load_abs        = 0;
    int _last_stall_direction       = 0;
    int _stall_confirm_count        = 0;
    bool _last_stall_feedback_valid = false;

    static int abs_int(int value)
    {
        return value < 0 ? -value : value;
    }

    bool is_raw_pos_valid(int raw_pos) const
    {
        return raw_pos >= _config.rawPosLimit.x && raw_pos <= _config.rawPosLimit.y;
    }

    int raw_pos_to_angle(int raw_pos) const
    {
        return (raw_pos - _zero_pos) * 5 * 10 / 16;
    }

    void reset_runtime_limits()
    {
        _runtime_raw_pos_limit = _config.rawPosLimit;
        set_angle_limit(_config.angleLimit);
        reset_stall_detection();
    }

    void reset_stall_detection()
    {
        _last_stall_feedback_valid = false;
        _last_stall_direction      = 0;
        _stall_confirm_count       = 0;
    }

    bool update_stall_protection(int target_raw_pos)
    {
        if (!_config.enableStallProtection) {
            return false;
        }

        const uint32_t now = GetHAL().millis();
        if (now - _last_stall_check_tick < kStallFeedbackIntervalMs) {
            return false;
        }
        _last_stall_check_tick = now;

        if (_scs_bus.FeedBack(_config.id) < 0) {
            reset_stall_detection();
            return false;
        }

        const int current_raw_pos = _scs_bus.ReadPos(-1);
        const int current_abs     = abs_int(_scs_bus.ReadCurrent(-1));
        const int load_abs        = abs_int(_scs_bus.ReadLoad(-1));

        if (!is_raw_pos_valid(current_raw_pos)) {
            reset_stall_detection();
            return false;
        }

        const int target_delta = target_raw_pos - current_raw_pos;
        if (abs_int(target_delta) < kStallMinTargetDeltaRaw) {
            reset_stall_detection();
            return false;
        }

        const int direction = target_delta > 0 ? 1 : -1;
        if (_last_stall_feedback_valid && direction == _last_stall_direction) {
            const int pos_delta = abs_int(current_raw_pos - _last_stall_raw_pos);
            const bool position_stuck = pos_delta <= kStallMaxPositionDeltaRaw;
            const bool current_spike  = current_abs >= kStallCurrentAbsThreshold ||
                                       current_abs - _last_stall_current_abs >= kStallCurrentRiseThreshold;
            const bool load_spike = load_abs >= kStallLoadAbsThreshold ||
                                    load_abs - _last_stall_load_abs >= kStallLoadRiseThreshold;

            if (position_stuck && (current_spike || load_spike)) {
                _stall_confirm_count++;
            } else if (pos_delta > kStallMaxPositionDeltaRaw) {
                _stall_confirm_count = 0;
            }
        } else {
            _stall_confirm_count = 0;
        }

        _last_stall_raw_pos         = current_raw_pos;
        _last_stall_current_abs     = current_abs;
        _last_stall_load_abs        = load_abs;
        _last_stall_direction       = direction;
        _last_stall_feedback_valid = true;

        if (_stall_confirm_count < kStallConfirmSamples) {
            return false;
        }

        handle_stall(current_raw_pos, direction, current_abs, load_abs);
        return true;
    }

    void handle_stall(int raw_pos, int direction, int current_abs, int load_abs)
    {
        int angle = raw_pos_to_angle(raw_pos);
        angle     = uitk::clamp(angle, _config.angleLimit.x, _config.angleLimit.y);

        auto angle_limit = getAngleLimit();
        if (direction > 0) {
            if (raw_pos < _runtime_raw_pos_limit.y) {
                _runtime_raw_pos_limit.y = raw_pos;
            }
            if (angle < angle_limit.y) {
                angle_limit.y = angle;
            }
        } else {
            if (raw_pos > _runtime_raw_pos_limit.x) {
                _runtime_raw_pos_limit.x = raw_pos;
            }
            if (angle > angle_limit.x) {
                angle_limit.x = angle;
            }
        }
        set_angle_limit(angle_limit);
        stop_motion_at_angle(angle);
        reset_stall_detection();

        check_mode(Mode::Position);
        _scs_bus.WritePos(_config.id, raw_pos, 20, 0);

        mclog::tagWarn(_tag,
                       "id: {} stall detected, raw: {}, angle: {}, dir: {}, current: {}, load: {}, limit: [{}, {}]",
                       _config.id, raw_pos, angle, direction, current_abs, load_abs, angle_limit.x, angle_limit.y);
    }

    void check_mode(Mode targetMode)
    {
        if (targetMode == _current_mode) return;
        _scs_bus.SwitchMode(_config.id, static_cast<uint8_t>(targetMode));
        _current_mode = targetMode;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// probe_servo_axis – returns true if the servo responds on the bus
// Timeout is controlled by SCSerial::IOTimeOut (default 100 ms).
// ─────────────────────────────────────────────────────────────────────────────
static bool probe_servo_axis(int id)
{
    int pos = _scs_bus.ReadPos(id);
    return (pos >= 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// disable_uart_and_pullup_gpio
// Called when both axes are Unavailable to prevent floating RX noise triggering
// UART interrupts (§6.4.1 rule 4).
// ─────────────────────────────────────────────────────────────────────────────
static void disable_uart_and_pullup_gpio()
{
    uart_driver_delete(SERVO_UART_NUM);

    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask  = (1ULL << SERVO_TX_PIN) | (1ULL << SERVO_RX_PIN);
    io_conf.mode          = GPIO_MODE_INPUT;
    io_conf.pull_up_en    = GPIO_PULLUP_ENABLE;
    io_conf.pull_down_en  = GPIO_PULLDOWN_DISABLE;
    io_conf.intr_type     = GPIO_INTR_DISABLE;
    gpio_config(&io_conf);
}

// ─────────────────────────────────────────────────────────────────────────────
// Hal::initServos  (§6.4)
// ─────────────────────────────────────────────────────────────────────────────
void Hal::initServos()
{
    static const char* TAG = "HAL-Servo";
    mclog::tagInfo(TAG, "init");

    // ── Step 1: Install UART and initialise SCS bus ──────────────────────────
    _scs_bus.begin(SERVO_UART_NUM, SERVO_BAUD, SERVO_TX_PIN, SERVO_RX_PIN);

    // ── Step 2: Probe each axis (unless force-disabled) ──────────────────────
    bool yaw_found   = false;
    bool pitch_found = false;

    if (_registry.isAvailable(DeviceCapability::ServoYaw)) {
        yaw_found = probe_servo_axis(SERVO_YAW_ID);
        if (yaw_found) {
            mclog::tagInfo(TAG, "Yaw servo (ID={}) detected", SERVO_YAW_ID);
        } else {
            mclog::tagWarn(TAG, "Yaw servo (ID={}) not detected", SERVO_YAW_ID);
            _registry.set(DeviceCapability::ServoYaw, false);
            _registry.setUnavailableReason(DeviceCapability::ServoYaw, "not detected");
        }
    } else {
        mclog::tagInfo(TAG, "Yaw servo skipped (force-disabled)");
    }

    if (_registry.isAvailable(DeviceCapability::ServoPitch)) {
        pitch_found = probe_servo_axis(SERVO_PITCH_ID);
        if (pitch_found) {
            mclog::tagInfo(TAG, "Pitch servo (ID={}) detected", SERVO_PITCH_ID);
        } else {
            mclog::tagWarn(TAG, "Pitch servo (ID={}) not detected", SERVO_PITCH_ID);
            _registry.set(DeviceCapability::ServoPitch, false);
            _registry.setUnavailableReason(DeviceCapability::ServoPitch, "not detected");
        }
    } else {
        mclog::tagInfo(TAG, "Pitch servo skipped (force-disabled)");
    }

    // ── Step 3: UART cleanup if both axes unavailable ────────────────────────
    if (!yaw_found && !pitch_found) {
        mclog::tagWarn(TAG, "No servos detected – disabling UART, applying GPIO pull-up");
        disable_uart_and_pullup_gpio();
    }

    // ── Step 4: Update capability registry ──────────────────────────────────
    _registry.set(DeviceCapability::ServoYaw,   yaw_found);
    _registry.set(DeviceCapability::ServoPitch, pitch_found);

    // ── Step 5: Build Motion instance (always created, never null) ───────────
    // Servo configs
    ServoConfig_t yaw_cfg;
    yaw_cfg.id                     = SERVO_YAW_ID;
    yaw_cfg.defaultZeroPos         = 460;
    yaw_cfg.angleLimit             = Vector2i(-1280, 1280);
    yaw_cfg.rawPosLimit            = Vector2i(0, 1000);
    yaw_cfg.settingNs              = "servo";
    yaw_cfg.settingZeroPositionKey = "zero_pos_1";
    yaw_cfg.enablePwmMode          = true;

    ServoConfig_t pitch_cfg;
    pitch_cfg.id                     = SERVO_PITCH_ID;
    pitch_cfg.defaultZeroPos         = 620;
    pitch_cfg.angleLimit             = Vector2i(30, 870);
    pitch_cfg.rawPosLimit            = Vector2i(0, 1000);
    pitch_cfg.settingNs              = "servo";
    pitch_cfg.settingZeroPositionKey = "zero_pos_2";
    pitch_cfg.enableStallProtection  = true;

    // Only create ScsServo instances for axes that are physically present.
    std::unique_ptr<Servo> yaw_servo   = yaw_found   ? std::make_unique<ScsServo>(yaw_cfg)   : nullptr;
    std::unique_ptr<Servo> pitch_servo = pitch_found ? std::make_unique<ScsServo>(pitch_cfg) : nullptr;

    auto motion = std::make_unique<Motion>(std::move(yaw_servo), std::move(pitch_servo));
    motion->init();

    GetStackChan().attachMotion(std::move(motion));

    mclog::tagInfo(TAG, "init done (yaw={} pitch={})", yaw_found ? "OK" : "absent",
                   pitch_found ? "OK" : "absent");
}
