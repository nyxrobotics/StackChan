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
static std::atomic<bool>     s_localTtsActive{false};  // TTS再生中フラグ（タッチ中断判定用）
static std::atomic<bool>     s_convReady{false};        // conv 初期化完了フラグ

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

void notify_xiaozhi_connected() {
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_cbs.onXiaozhiConnected) s_cbs.onXiaozhiConnected();
}

void notify_xiaozhi_disconnected() {
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_cbs.onXiaozhiDisconnected) s_cbs.onXiaozhiDisconnected();
}

void notify_xiaozhi_error() {
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_cbs.onXiaozhiError) s_cbs.onXiaozhiError();
}

void notify_turn_start() {
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_cbs.onTurnStart) s_cbs.onTurnStart();
}

void notify_turn_end() {
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_cbs.onTurnEnd) s_cbs.onTurnEnd();
}

void notify_audio_chunk(const uint8_t* pcm, size_t len) {
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_cbs.onAudioChunk) s_cbs.onAudioChunk(pcm, len);
}

void notify_local_tts_start() {
    s_localTtsActive.store(true);
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_cbs.onLocalTtsStart) s_cbs.onLocalTtsStart();
}

void notify_local_tts_end() {
    s_localTtsActive.store(false);
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_cbs.onLocalTtsEnd) s_cbs.onLocalTtsEnd();
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
    s_localTtsActive.store(false);
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_cbs.onLocalAbort) s_cbs.onLocalAbort();
}

} // namespace hal_bridge
