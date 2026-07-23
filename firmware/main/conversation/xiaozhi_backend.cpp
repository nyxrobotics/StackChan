#include "xiaozhi_backend.h"

#include <esp_log.h>

// ---------------------------------------------------------------------------
// The actual Xiaozhi protocol implementation lives in the original
// StackChan / xiaozhi-esp32 codebase.  This backend is intentionally
// a thin delegation layer so that the existing logic is preserved unchanged
// (Design Principle 2.1).
//
// Replace the TODO stubs below with calls into the real Xiaozhi API once
// this file is integrated into the firmware tree.
// ---------------------------------------------------------------------------

static const char* TAG = "XiaozhiBackend";

XiaozhiBackend::XiaozhiBackend() {
    ESP_LOGD(TAG, "created");
}

XiaozhiBackend::~XiaozhiBackend() = default;

void XiaozhiBackend::start() {
    ESP_LOGI(TAG, "start");
    // TODO: ensure Xiaozhi WebSocket is open
    // Existing StackChan AI.AGENT code manages the connection lifecycle;
    // nothing extra needed here unless the socket was explicitly closed.
}

void XiaozhiBackend::stop() {
    ESP_LOGI(TAG, "stop");
    // TODO: flush any pending Xiaozhi requests if needed
    // Do NOT close the WebSocket — it may be re-activated next turn.
}

void XiaozhiBackend::beginTurn() {
    ESP_LOGD(TAG, "beginTurn");
    // TODO: notify Xiaozhi session that a new turn is starting
    // (session_id / turn tracking if the protocol requires it)
}

void XiaozhiBackend::endTurn() {
    ESP_LOGD(TAG, "endTurn");
    // TODO: finalise Xiaozhi turn — flush audio, wait for EOS, etc.
}

void XiaozhiBackend::processText(const std::string& text) {
    ESP_LOGD(TAG, "processText: %s", text.c_str());
    // TODO: send text message to Xiaozhi WebSocket
    // Ref: xiaozhi-esp32 docs/websocket.md — "text" message type
}

void XiaozhiBackend::processAudio(const uint8_t* pcm, size_t len) {
    ESP_LOGD(TAG, "processAudio: %zu bytes", len);
    // TODO: stream PCM to Xiaozhi WebSocket audio endpoint
    // Existing app_ai_agent.cpp already does this; delegate here.
    (void)pcm;
    (void)len;
}
