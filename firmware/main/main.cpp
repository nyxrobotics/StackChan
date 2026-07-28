/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include <smooth_ui_toolkit.hpp>
#include <uitk/short_namespace.hpp>
#include <mooncake_log.h>
#include <mooncake.h>
#include <apps/apps.h>
#include <hal/hal.h>
#include <esp_attr.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>

using namespace mooncake;
using namespace smooth_ui_toolkit;

namespace {

constexpr const char* kRecoveryTag = "ai_agent_recovery";
constexpr uint32_t kAiAgentRecoveryStateSignature = 0x41494147;  // "AIAG"
constexpr uint64_t kRecoveryStableAfterUs = 60ULL * 1000 * 1000;

enum class AiAgentRecoveryPhase : uint32_t {
    Inactive = 0,
    Running,
    Recovering,
};

struct AiAgentRecoveryState {
    uint32_t signature;
    AiAgentRecoveryPhase phase;
};

RTC_NOINIT_ATTR AiAgentRecoveryState ai_agent_recovery_state;
esp_timer_handle_t recovery_stable_timer = nullptr;

bool isCrashReset(esp_reset_reason_t reason)
{
    switch (reason) {
        case ESP_RST_PANIC:
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT:
        case ESP_RST_BROWNOUT:
            return true;
        default:
            return false;
    }
}

void resetRecoveryState()
{
    ai_agent_recovery_state.signature = kAiAgentRecoveryStateSignature;
    ai_agent_recovery_state.phase = AiAgentRecoveryPhase::Inactive;
}

bool shouldRecoverAiAgent()
{
    const esp_reset_reason_t reset_reason = esp_reset_reason();
    if (ai_agent_recovery_state.signature != kAiAgentRecoveryStateSignature) {
        resetRecoveryState();
    }

    if (!isCrashReset(reset_reason)) {
        resetRecoveryState();
        return false;
    }

    if (ai_agent_recovery_state.phase == AiAgentRecoveryPhase::Running) {
        ai_agent_recovery_state.phase = AiAgentRecoveryPhase::Recovering;
        ESP_LOGW(kRecoveryTag, "AI Agent crashed (reset reason %d); recovering once",
                 static_cast<int>(reset_reason));
        return true;
    }

    if (ai_agent_recovery_state.phase == AiAgentRecoveryPhase::Recovering) {
        ESP_LOGE(kRecoveryTag, "AI Agent crashed again during recovery; opening launcher");
    }
    resetRecoveryState();
    return false;
}

void markAiAgentStarting(bool recovering)
{
    ai_agent_recovery_state.signature = kAiAgentRecoveryStateSignature;
    if (!recovering) {
        ai_agent_recovery_state.phase = AiAgentRecoveryPhase::Running;
    }
}

void armRecoveryBootLoopGuard(bool recovering)
{
    if (!recovering) {
        return;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = [](void*) {
            ai_agent_recovery_state.signature = kAiAgentRecoveryStateSignature;
            ai_agent_recovery_state.phase = AiAgentRecoveryPhase::Running;
            ESP_LOGI(kRecoveryTag, "AI Agent recovery remained stable; crash recovery re-armed");
        },
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "ai_recovery_guard",
        .skip_unhandled_events = true,
    };

    esp_err_t error = esp_timer_create(&timer_args, &recovery_stable_timer);
    if (error == ESP_OK) {
        error = esp_timer_start_once(recovery_stable_timer, kRecoveryStableAfterUs);
    }
    if (error != ESP_OK) {
        ESP_LOGE(kRecoveryTag, "Failed to arm boot-loop guard: %s", esp_err_to_name(error));
    }
}

}  // namespace

extern "C" void app_main(void)
{
    // Setup logger
    mclog::set_level(mclog::level_info);
    mclog::set_time_format(mclog::time_format_unix_milliseconds);

    // HAL init
    GetHAL().init();

    // Setup ui hal
    ui_hal::on_delay([](uint32_t ms) { GetHAL().delay(ms); });
    ui_hal::on_get_tick([]() { return GetHAL().millis(); });

#ifdef CONFIG_STACKCHAN_TEST_AUTO_START_AI_AGENT
    constexpr bool forceAiAgentForUnattendedTest = true;
#else
    constexpr bool forceAiAgentForUnattendedTest = false;
#endif
    const bool recoverAiAgent = shouldRecoverAiAgent();
    const bool skip_mooncake =
        forceAiAgentForUnattendedTest ||
        recoverAiAgent ||
        (GetHAL().getXiaozhiConfig().startAiAgentOnBoot &&
         GetHAL().getWarmRebootTarget() < 0);

    if (!skip_mooncake) {
        // Install apps
        GetMooncake().installApp(std::make_unique<AppLauncher>());
        GetMooncake().installApp(std::make_unique<AppAiAgent>());
        GetMooncake().installApp(std::make_unique<AppAvatar>());
        GetMooncake().installApp(std::make_unique<AppEspnowControl>());
        GetMooncake().installApp(std::make_unique<AppAppCenter>());
        GetMooncake().installApp(std::make_unique<AppEzdata>());
        GetMooncake().installApp(std::make_unique<AppDance>());
        GetMooncake().installApp(std::make_unique<AppSetup>());

        // Main loop
        while (1) {
            GetHAL().feedTheDog();
            GetHAL().updateHeapStatusLog();

            GetMooncake().update();

            if (GetHAL().isXiaozhiStartRequested()) {
                break;
            }
        }

        // Uninstall all apps and destroy mooncake
        GetMooncake().uninstallAllApps();
        DestroyMooncake();
    }

    // Start xiaozhi, never returns
    markAiAgentStarting(recoverAiAgent);
    armRecoveryBootLoopGuard(recoverAiAgent);
    GetHAL().startXiaozhi();
}
