/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "motion.h"
#include "motion_math.h"

using namespace uitk;
using namespace stackchan::motion;

// ─────────────────────────────────────────────────────────────────────────────
// init / update
// ─────────────────────────────────────────────────────────────────────────────

void Motion::init()
{
    if (_yaw_servo)   _yaw_servo->init();
    if (_pitch_servo) _pitch_servo->init();
}

void Motion::update()
{
    if (_yaw_servo)   _yaw_servo->update();
    if (_pitch_servo) _pitch_servo->update();
}

// ─────────────────────────────────────────────────────────────────────────────
// Servo accessors (only called when callers already checked hasYaw/hasPitch)
// ─────────────────────────────────────────────────────────────────────────────

Servo& Motion::yawServo()
{
    return *_yaw_servo;
}

Servo& Motion::pitchServo()
{
    return *_pitch_servo;
}

// ─────────────────────────────────────────────────────────────────────────────
// Movement commands – no-op for absent axes
// ─────────────────────────────────────────────────────────────────────────────

void Motion::moveYaw(int angle)
{
    _last_yaw_target = angle;
    if (_yaw_servo) _yaw_servo->move(angle);
}

void Motion::moveYawWithSpeed(int angle, int speed)
{
    _last_yaw_target = angle;
    if (_yaw_servo) _yaw_servo->moveWithSpeed(angle, speed);
}

void Motion::movePitch(int angle)
{
    _last_pitch_target = angle;
    if (_pitch_servo) _pitch_servo->move(angle);
}

void Motion::movePitchWithSpeed(int angle, int speed)
{
    _last_pitch_target = angle;
    if (_pitch_servo) _pitch_servo->moveWithSpeed(angle, speed);
}

void Motion::move(int yawAngle, int pitchAngle)
{
    _last_yaw_target   = yawAngle;
    _last_pitch_target = pitchAngle;
    if (_yaw_servo)   _yaw_servo->move(yawAngle);
    if (_pitch_servo) _pitch_servo->move(pitchAngle);
}

void Motion::moveWithSpeed(int yawAngle, int pitchAngle, int speed)
{
    _last_yaw_target   = yawAngle;
    _last_pitch_target = pitchAngle;
    if (_yaw_servo)   _yaw_servo->moveWithSpeed(yawAngle, speed);
    if (_pitch_servo) _pitch_servo->moveWithSpeed(pitchAngle, speed);
}

void Motion::goHome(int speed)
{
    moveWithSpeed(0, 0, speed);
}

void Motion::stop()
{
    if (_yaw_servo)   _yaw_servo->move(_yaw_servo->getCurrentAngle());
    if (_pitch_servo) _pitch_servo->move(_pitch_servo->getCurrentAngle());
}

void Motion::lookAtNormalized(float x, float y, int speed)
{
    auto yaw_limit   = _yaw_servo ? _yaw_servo->getAngleLimit() : Vector2i(-1280, 1280);
    auto pitch_limit = _pitch_servo ? _pitch_servo->getAngleLimit() : Vector2i(30, 870);
    auto angles = calculateNormalizedLookAngles(x, y, yaw_limit.x, yaw_limit.y, pitch_limit.x, pitch_limit.y);
    moveWithSpeed(angles.yaw, angles.pitch, speed);
}

void Motion::lookAtPoint(float x, float y, float z, int speed)
{
    auto angles = calculatePointLookAngles(x, y, z);
    moveWithSpeed(angles.yaw, angles.pitch, speed);
}

// ─────────────────────────────────────────────────────────────────────────────
// State queries
// ─────────────────────────────────────────────────────────────────────────────

bool Motion::isMoving()
{
    bool yaw_moving   = _yaw_servo   && _yaw_servo->isMoving();
    bool pitch_moving = _pitch_servo && _pitch_servo->isMoving();
    return yaw_moving || pitch_moving;
}

int Motion::getCurrentYawAngle()
{
    // Return the physical angle if servo is present, otherwise the last
    // commanded target so that avatar animations stay coherent (§6.4.2).
    if (_yaw_servo) return _yaw_servo->getCurrentAngle();
    return _last_yaw_target;
}

int Motion::getCurrentPitchAngle()
{
    if (_pitch_servo) return _pitch_servo->getCurrentAngle();
    return _last_pitch_target;
}

uitk::Vector2i Motion::getCurrentAngles()
{
    return uitk::Vector2i(getCurrentYawAngle(), getCurrentPitchAngle());
}

void Motion::setTorqueEnabled(bool enabled)
{
    if (_yaw_servo)   _yaw_servo->setTorqueEnabled(enabled);
    if (_pitch_servo) _pitch_servo->setTorqueEnabled(enabled);
}

void Motion::setAutoTorqueReleaseEnabled(bool enabled)
{
    if (_yaw_servo)   _yaw_servo->setAutoTorqueReleaseEnabled(enabled);
    if (_pitch_servo) _pitch_servo->setAutoTorqueReleaseEnabled(enabled);
}

void Motion::setAutoAngleSyncEnabled(bool enabled)
{
    if (_yaw_servo)   _yaw_servo->setAutoAngleSyncEnabled(enabled);
    if (_pitch_servo) _pitch_servo->setAutoAngleSyncEnabled(enabled);
}

void Motion::setModifyLock(bool locked)
{
    _is_modify_locked = locked;
}

bool Motion::isModifyLocked()
{
    return _is_modify_locked;
}
