// firmware/main/hal/hal_bridge_conv.cpp

#include "hal_bridge_conv.h"
#include <esp_log.h>
#include <mutex>
#include <atomic>

static const char* TAG = "hal_bridge_conv";

namespace hal_bridge {

static std::mutex            s_mutex;
static ConversationCallbacks s_cbs;
static int                   s_mode = kModeAuto;
static std::atomic<int>      s_activeBackend{kBackendStatic};
static std::atomic<bool>     s_localTtsActive{false};  // TTS再生中フラグ（タッチ中断判定用）
static std::atomic<bool>     s_convReady{false};        // conv 初期化完了フラグ
static std::atomic<bool>     s_localModelLoading{false};

void set_conversation_callbacks(ConversationCallbacks cbs) {
    std::lock_guard<std::mutex> lock(s_mutex);
    s_cbs = std::move(cbs);
    ESP_LOGI(TAG, "conversation callbacks %s",
             s_cbs.onTurnStart ? "registered" : "cleared");
}

void set_conversation_mode(int mode) {
    std::lock_guard<std::mutex> lock(s_mutex);
    s_mode = mode;
    ESP_LOGI(TAG, "conversation mode = %d", mode);
}

int get_conversation_mode() {
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_mode;
}

void set_active_conversation_backend(int backend) {
    s_activeBackend.store(backend);
    ESP_LOGI(TAG, "active conversation backend = %d", backend);
}

int get_active_conversation_backend() {
    return s_activeBackend.load();
}

bool is_module_llm_backend_active() {
    return s_activeBackend.load() == kBackendModuleLLM;
}

void notify_network_connected() {
    std::function<void()> cb;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        cb = s_cbs.onNetworkConnected;
    }
    if (cb) cb();
}

void notify_network_disconnected() {
    std::function<void()> cb;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        cb = s_cbs.onNetworkDisconnected;
    }
    if (cb) cb();
}

void notify_xiaozhi_connected() {
    std::function<void()> cb;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        cb = s_cbs.onXiaozhiConnected;
    }
    if (cb) cb();
}

void notify_xiaozhi_disconnected() {
    std::function<void()> cb;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        cb = s_cbs.onXiaozhiDisconnected;
    }
    if (cb) cb();
}

void notify_xiaozhi_error() {
    std::function<void()> cb;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        cb = s_cbs.onXiaozhiError;
    }
    if (cb) cb();
}

void notify_turn_start() {
    std::function<void()> cb;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        cb = s_cbs.onTurnStart;
    }
    if (cb) cb();
}

void notify_turn_end() {
    std::function<void()> cb;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        cb = s_cbs.onTurnEnd;
    }
    if (cb) cb();
}

void notify_audio_chunk(const uint8_t* pcm, size_t len) {
    std::function<void(const uint8_t*, size_t)> cb;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        cb = s_cbs.onAudioChunk;
    }
    if (cb) cb(pcm, len);
}

void notify_local_tts_start() {
    s_localTtsActive.store(true);
    std::function<void()> cb;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        cb = s_cbs.onLocalTtsStart;
    }
    if (cb) cb();
}

void notify_local_tts_end() {
    s_localTtsActive.store(false);
    std::function<void()> cb;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        cb = s_cbs.onLocalTtsEnd;
    }
    if (cb) cb();
}

bool is_local_tts_active() {
    return s_localTtsActive.load();
}

void set_conv_ready(bool ready) {
    s_convReady.store(ready);
}

bool is_conv_ready() {
    return s_convReady.load();
}

void notify_local_abort() {
    std::function<void()> cb;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        cb = s_cbs.onLocalAbort;
    }
    if (cb) cb();
}

void notify_local_model_state(LocalModelVisualState state) {
    s_localModelLoading.store(state == LocalModelVisualState::Loading, std::memory_order_relaxed);

    std::function<void(LocalModelVisualState)> cb;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        cb = s_cbs.onLocalModelStateChanged;
    }
    if (cb) cb(state);
}

bool is_local_model_loading() {
    return s_localModelLoading.load(std::memory_order_relaxed);
}

} // namespace hal_bridge
