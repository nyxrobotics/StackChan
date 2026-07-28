#include "conversation_runtime.h"

#include "conversation_manager.h"
#include "module_llm_backend.h"

#include "application.h"
#include "assets/lang_config.h"
#include "board.h"
#include "display/display.h"
#include "hal/hal_bridge_conv.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <memory>

static const char* TAG = "ConvRuntime";

namespace conversation_runtime {
namespace {

std::unique_ptr<ConversationManager> s_manager;
TaskHandle_t s_startTask = nullptr;
bool s_prepared = false;

void refreshLocalResponseDisplay(DeviceState target, const char* phase)
{
    auto display = Board::GetInstance().GetDisplay();
    if (!display) {
        ESP_LOGW(TAG, "local response %s: display unavailable", phase);
        return;
    }

    if (target == kDeviceStateSpeaking) {
        display->SetStatus(Lang::Strings::SPEAKING);
        ESP_LOGI(TAG, "local response %s: speaking display refreshed", phase);
    } else if (target == kDeviceStateListening) {
        display->SetStatus(Lang::Strings::LISTENING);
        ESP_LOGI(TAG, "local response %s: listening display refreshed", phase);
    }
}

void setLocalResponseState(DeviceState target, const char* phase)
{
    auto& app = Application::GetInstance();
    DeviceState before = app.GetDeviceState();
    if (before == target) {
        ESP_LOGI(TAG, "local response %s: already state=%d", phase, static_cast<int>(target));
        refreshLocalResponseDisplay(target, phase);
        return;
    }

    bool ok = app.SetDeviceState(target);
    ESP_LOGI(TAG, "local response %s: state %d -> %d %s",
             phase, static_cast<int>(before), static_cast<int>(target),
             ok ? "ok" : "rejected");
    if (ok) {
        refreshLocalResponseDisplay(target, phase);
    }
}

void registerCallbacks()
{
    hal_bridge::set_conversation_callbacks({
        .onNetworkConnected = []() {
            if (s_manager) s_manager->onNetworkConnected();
        },
        .onNetworkDisconnected = []() {
            if (s_manager) s_manager->onNetworkDisconnected();
        },
        .onXiaozhiConnected = []() {
            if (s_manager) s_manager->onXiaozhiConnected();
        },
        .onXiaozhiDisconnected = []() {
            if (s_manager) s_manager->onXiaozhiDisconnected();
        },
        .onXiaozhiError = []() {
            if (s_manager) s_manager->onXiaozhiError();
        },
        .onTurnStart = []() {
            if (s_manager) s_manager->onTurnStart();
        },
        .onTurnEnd = []() {
            if (s_manager) s_manager->onTurnEnd();
        },
        .onAudioChunk = [](const uint8_t* pcm, size_t len) {
            if (s_manager && s_manager->activeBackendKind() == BackendKind::ModuleLLM) {
                s_manager->processAudio(pcm, len);
            }
        },
        .onLocalTtsStart = []() {
            setLocalResponseState(kDeviceStateSpeaking, "start");
        },
        .onLocalTtsEnd = []() {
            auto state = Application::GetInstance().GetDeviceState();
            if (state == kDeviceStateSpeaking) {
                setLocalResponseState(kDeviceStateListening, "end");
            }
        },
        .onLocalAbort = []() {
            if (!s_manager) return;
            auto* backend = s_manager->getModuleLLMBackend();
            if (backend) backend->abortSpeaking();
        },
    });
}

void startTask(void*)
{
    if (s_manager) {
        s_manager->start();
    }
    hal_bridge::set_conv_ready(true);
    ESP_LOGI(TAG, "conversation runtime ready");
    s_startTask = nullptr;
    vTaskDelete(nullptr);
}

}  // namespace

void prepare()
{
    auto mode = ConversationManager::loadModeFromNvs();

    if (!s_manager) {
        s_manager = std::make_unique<ConversationManager>(mode);
    } else if (s_manager->mode() != mode) {
        s_manager->setMode(mode);
    }

    hal_bridge::set_conversation_mode(static_cast<int>(s_manager->mode()));
    registerCallbacks();

    NetworkStartupPolicy networkPolicy = NetworkStartupPolicy::Optional;
    if (s_manager->mode() == ConversationMode::LocalOnly) {
        networkPolicy = NetworkStartupPolicy::Disabled;
    } else if (s_manager->mode() == ConversationMode::OnlineOnly) {
        networkPolicy = NetworkStartupPolicy::Required;
    }
    Application::GetInstance().SetNetworkStartupPolicy(networkPolicy);
    hal_bridge::set_conv_ready(false);
    s_prepared = true;

    ESP_LOGI(TAG, "prepared conversation runtime: mode=%d network_policy=%d",
             static_cast<int>(s_manager->mode()), static_cast<int>(networkPolicy));
}

void start()
{
    if (!s_prepared) {
        prepare();
    }

    ESP_LOGI(TAG, "starting conversation runtime");

    if (s_startTask != nullptr) {
        ESP_LOGI(TAG, "conversation runtime start already in progress");
        return;
    }

    BaseType_t created = xTaskCreate(startTask, "conv_start", 32768, nullptr, 5, &s_startTask);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "failed to create conv_start task");
        s_startTask = nullptr;
        hal_bridge::set_conv_ready(true);
    }
}

ConversationManager* manager()
{
    return s_manager.get();
}

}  // namespace conversation_runtime
