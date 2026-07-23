/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

// ============================================================
// Original file: firmware/main/apps/app_ai_agent/app_ai_agent.cpp
//
// Changes from original:
//   [ADD] Include conversation_manager.h
//   [ADD] Static ConversationManager instance (conv_)
//   [ADD] onOpen(): conv_.start() before requestXiaozhiStart()
//   [ADD] onOpen(): Register hal_bridge Xiaozhi event callbacks
//   [ADD] onClose(): conv_.stop()
//   [KEEP] All original Xiaozhi behavior unchanged
// ============================================================

#include "app_ai_agent.h"
#include <hal/hal.h>
#include <mooncake.h>
#include <mooncake_log.h>
#include <assets/assets.h>
#include <smooth_lvgl.hpp>
#include <stackchan/stackchan.h>
#include <apps/common/common.h>

// [ADD] Include conversation_manager.h
#include "conversation/conversation_manager.h"
#include "hal/board/hal_bridge.h"  // for Xiaozhi event callback registration
#include "hal/hal_bridge_conv.h"  // set_conversation_callbacks
#include "application.h"
#include "conversation/module_llm_backend.h"  // abortSpeaking
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

using namespace mooncake;
using namespace smooth_ui_toolkit::lvgl_cpp;

// [ADD] Module-level ConversationManager (lives for the lifetime of the app)
static ConversationManager* s_conv = nullptr;

static void setLocalResponseState(DeviceState target, const char* phase)
{
    auto& app = Application::GetInstance();
    DeviceState before = app.GetDeviceState();
    if (before == target) {
        mclog::tagInfo("AI.AGENT", "local response {}: already state={}", phase, static_cast<int>(target));
        return;
    }

    bool ok = app.SetDeviceState(target);
    mclog::tagInfo("AI.AGENT", "local response {}: state {} -> {} {}",
                   phase, static_cast<int>(before), static_cast<int>(target),
                   ok ? "ok" : "rejected");
}

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

    // [ADD] Initialize ConversationManager — mode loaded from NVS (default: Auto)
    if (s_conv == nullptr) {
        auto mode = ConversationManager::loadModeFromNvs();
        s_conv = new ConversationManager(mode);
    } else {
        // Already initialized (warm reboot): re-apply mode in case it was changed in settings
        s_conv->setMode(ConversationManager::loadModeFromNvs());
    }

    // Publish active mode to hal_bridge so application.cc hooks can gate Xiaozhi
    hal_bridge::set_conversation_mode(static_cast<int>(s_conv->mode()));

    // [ADD] LocalOnly モードでは Wi-Fi と Xiaozhi プロトコルを無効化する
    if (s_conv->mode() == ConversationMode::LocalOnly) {
        Application::GetInstance().SetNetworkRequired(false);
    } else {
        Application::GetInstance().SetNetworkRequired(true);
    }

    // [ADD] Wire Xiaozhi lifecycle events — callbacks を先に登録してから start()
    hal_bridge::set_conversation_callbacks({
        // Called by hal_bridge when Xiaozhi WebSocket connects (onlineReady = true)
        .onXiaozhiConnected = []() {
            if (s_conv) s_conv->onXiaozhiConnected();
        },
        // Called by hal_bridge when Xiaozhi WebSocket disconnects
        .onXiaozhiDisconnected = []() {
            if (s_conv) s_conv->onXiaozhiDisconnected();
        },
        // Called by hal_bridge when Xiaozhi reports a protocol error
        .onXiaozhiError = []() {
            if (s_conv) s_conv->onXiaozhiError();
        },
        // Called by hal_bridge at the start of each voice turn (VAD triggered)
        .onTurnStart = []() {
            if (s_conv) s_conv->onTurnStart();
        },
        // Called by hal_bridge when the turn (ASR+LLM+TTS+playback) is complete
        .onTurnEnd = []() {
            if (s_conv) s_conv->onTurnEnd();
        },
        // Called by hal_bridge with each audio chunk captured from the microphone.
        .onAudioChunk = [](const uint8_t* pcm, size_t len) {
            if (s_conv && s_conv->activeBackendKind() == BackendKind::ModuleLLM) {
                s_conv->processAudio(pcm, len);
            }
        },
        // [ADD] Local mode: response generation start -> speaking animation.
        // This is intentionally earlier than audio playback, matching cloud mode.
        .onLocalTtsStart = []() {
            setLocalResponseState(kDeviceStateSpeaking, "start");
        },
        // [ADD] Local mode: response generation/playback end -> Listening state.
        .onLocalTtsEnd = []() {
            auto state = Application::GetInstance().GetDeviceState();
            if (state == kDeviceStateSpeaking) {
                setLocalResponseState(kDeviceStateListening, "end");
            }
        },
        // [ADD] Local mode: 顔タッチ中断 → backend に abort 通知
        .onLocalAbort = []() {
            if (s_conv) {
                auto* backend = s_conv->getModuleLLMBackend();
                if (backend) backend->abortSpeaking();
            }
        },
    });

    // 顔を先に表示（ActivationTask が is_conv_ready() を待つ間 kDeviceStateActivating）
    hal_bridge::set_conv_ready(false);
    GetHAL().requestXiaozhiStart();

    // モデル読み込みはバックグラウンドタスクで実行
    BaseType_t created = xTaskCreate([](void*) {
        if (s_conv) s_conv->start();
        hal_bridge::set_conv_ready(true);  // ActivationTask のブロック解除 → popup 再生
        vTaskDelete(nullptr);
    }, "conv_start", 32768, nullptr, 5, nullptr);
    if (created != pdPASS) {
        mclog::tagError(getAppInfo().name, "failed to create conv_start task");
        hal_bridge::set_conv_ready(true);
    }
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

    // [ADD] Clear callbacks so hal_bridge doesn't call into a stopped manager
    hal_bridge::set_conversation_callbacks({});

    // [ADD] Stop ConversationManager (deactivates active backend between turns)
    if (s_conv) {
        s_conv->stop();
    }
}
