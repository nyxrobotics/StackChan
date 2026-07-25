/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_dance.h"
#include <hal/hal.h>
#include <mooncake.h>
#include <mooncake_log.h>
#include <stackchan/stackchan.h>
#include <apps/common/common.h>
#include <assets/assets.h>

using namespace mooncake;
using namespace stackchan;

AppDance::AppDance()
{
    // 配置 App 名
    setAppInfo().name = "DANCE";
    // 配置 App 图标
    static auto icon  = assets::get_image("icon_dance.bin");
    setAppInfo().icon = (void*)&icon;
    // 配置 App 主题颜色
    static uint32_t theme_color = 0xB77BFF;
    setAppInfo().userData       = (void*)&theme_color;
}

// App 被安装时会被调用
void AppDance::onCreate()
{
    mclog::tagInfo(getAppInfo().name, "on create");
}

void AppDance::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "on open");

    // Create loading page
    std::unique_ptr<view::LoadingPage> loading_page;
    {
        LvglLockGuard lock;
        loading_page = std::make_unique<view::LoadingPage>(0xB77BFF, 0x422268);
        loading_page->setMessage("Starting\n BLE server...");
    }

    // Start BLE service. A different BLE profile may already own the NimBLE
    // host; live profile switching is deliberately rejected because the ESP-IDF
    // stop path can wait forever.
    const bool ble_server_started = GetHAL().startBleServer();

    LvglLockGuard lock;

    // Destroy loading page
    loading_page.reset();

    // Create selected avatar skin
    auto selected_avatar = avatar::createSelectedAvatar(lv_screen_active());
    GetStackChan().attachAvatar(std::move(selected_avatar));

    /* ------------------------------- BLE events ------------------------------- */
    if (ble_server_started) {
        GetHAL().onBleAvatarData.connect([&](const char* data) {
            std::lock_guard<std::mutex> lock(_mutex);
            if (_ble_avatar_data.update_flag) {
                return;
            }
            _ble_avatar_data.data        = data;
            _ble_avatar_data.update_flag = true;
        });

        GetHAL().onBleMotionData.connect([&](const char* data) {
            std::lock_guard<std::mutex> lock(_mutex);
            if (_ble_motion_data.update_flag) {
                return;
            }
            _ble_motion_data.data        = data;
            _ble_motion_data.update_flag = true;
        });

        GetHAL().onBleRgbData.connect([&](const char* data) {
            std::lock_guard<std::mutex> lock(_mutex);
            if (_ble_rgb_data.update_flag) {
                return;
            }
            _ble_rgb_data.data        = data;
            _ble_rgb_data.update_flag = true;
        });
    } else {
        mclog::tagError(getAppInfo().name, "BLE server unavailable: another BLE profile is active");
    }

    /* ----------------------------- Common widgets ----------------------------- */
    view::create_home_indicator([&]() { close(); }, 0xB77BFF, 0x422268);
    view::create_status_bar(0xB77BFF, 0x422268);
    if (!ble_server_started) {
        view::pop_a_toast("Bluetooth is already in use.\nRestart Stack-chan to switch modes.", view::ToastType::Error,
                          12000);
    }
}

void AppDance::onRunning()
{
    BleHandlerData_t ble_avatar_data;
    BleHandlerData_t ble_motion_data;
    BleHandlerData_t ble_rgb_data;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_ble_avatar_data.update_flag) {
            ble_avatar_data.data         = std::move(_ble_avatar_data.data);
            ble_avatar_data.update_flag  = true;
            _ble_avatar_data.update_flag = false;
        }
        if (_ble_motion_data.update_flag) {
            ble_motion_data.data         = std::move(_ble_motion_data.data);
            ble_motion_data.update_flag  = true;
            _ble_motion_data.update_flag = false;
        }
        if (_ble_rgb_data.update_flag) {
            ble_rgb_data.data         = std::move(_ble_rgb_data.data);
            ble_rgb_data.update_flag  = true;
            _ble_rgb_data.update_flag = false;
        }
    }

    LvglLockGuard lvgl_lock;

    if (ble_avatar_data.update_flag) {
        GetStackChan().updateAvatarFromJson(ble_avatar_data.data.c_str());
    }

    if (ble_motion_data.update_flag) {
        check_auto_angle_sync_mode();
        GetStackChan().updateMotionFromJson(ble_motion_data.data.c_str());
    }

    if (ble_rgb_data.update_flag) {
        GetStackChan().updateNeonLightFromJson(ble_rgb_data.data.c_str());
    }

    GetStackChan().update();

    view::update_home_indicator();
    view::update_status_bar();
}

void AppDance::onClose()
{
    mclog::tagInfo(getAppInfo().name, "on close");

    {
        LvglLockGuard lock;

        GetStackChan().resetAvatar();

        GetHAL().onBleAvatarData.clear();
        GetHAL().onBleMotionData.clear();
        GetHAL().onBleRgbData.clear();

        view::destroy_home_indicator();
        view::destroy_status_bar();
    }

    GetHAL().requestWarmReboot(5);
}

void AppDance::check_auto_angle_sync_mode()
{
    auto& motion = GetStackChan().motion();

    // If far from last command, enable auto angle sync
    if (GetHAL().millis() - _last_motion_cmd_tick > 2000) {
        motion.setAutoAngleSyncEnabled(true);
    } else {
        motion.setAutoAngleSyncEnabled(false);
    }

    _last_motion_cmd_tick = GetHAL().millis();
}
