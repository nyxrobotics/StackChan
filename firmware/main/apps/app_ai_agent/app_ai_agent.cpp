/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "app_ai_agent.h"
#include <hal/hal.h>
#include <mooncake.h>
#include <mooncake_log.h>
#include <assets/assets.h>

using namespace mooncake;
using namespace smooth_ui_toolkit::lvgl_cpp;

AppAiAgent::AppAiAgent()
{
    // Configure App name
    setAppInfo().name = "AI.AGENT";
    // Configure App icon
    static auto icon = assets::get_image("icon_ai_agent.bin");
    setAppInfo().icon = (void*)&icon;
    // Configure App theme color
    static uint32_t theme_color = 0x33CC99;
    setAppInfo().userData = (void*)&theme_color;
}

// Called when the App is installed
void AppAiAgent::onCreate()
{
    mclog::tagInfo(getAppInfo().name, "on create");
}

// Called when the App is opened
// You can construct UI, initialize operations, etc. here
void AppAiAgent::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "on open");
    GetHAL().requestXiaozhiStart();
}

// Called repeatedly while the App is running
void AppAiAgent::onRunning()
{
    // No polling needed: all state updates come through hal_bridge callbacks.
}

// Called when the App is closed
// You can destroy UI, release resources, etc. here
void AppAiAgent::onClose()
{
    mclog::tagInfo(getAppInfo().name, "on close");
}
