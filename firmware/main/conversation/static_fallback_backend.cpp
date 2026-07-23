#include "static_fallback_backend.h"

#include <esp_log.h>

static const char* TAG = "StaticFallback";

// Default fallback messages (Japanese — matches default language config)
static const std::vector<std::string> kDefaultMessages = {
    "すみません、今はネットワークに接続できません。",
    "オフラインモードです。しばらくお待ちください。",
    "接続が切断されています。再接続をお試しください。",
};

// ---------------------------------------------------------------------------

StaticFallbackBackend::StaticFallbackBackend()
    : messages_(kDefaultMessages)
{}

StaticFallbackBackend::~StaticFallbackBackend() = default;

void StaticFallbackBackend::start() {
    ESP_LOGI(TAG, "start");
}

void StaticFallbackBackend::stop() {
    ESP_LOGI(TAG, "stop");
}

void StaticFallbackBackend::beginTurn() {
    ESP_LOGD(TAG, "beginTurn");
}

void StaticFallbackBackend::endTurn() {
    ESP_LOGD(TAG, "endTurn");
}

void StaticFallbackBackend::processText(const std::string& text) {
    (void)text;
    const std::string& msg = pickMessage();
    ESP_LOGI(TAG, "fallback response: %s", msg.c_str());
    // TODO: play msg via TTS or display on screen
    // e.g. DisplayManager::showSubtitle(msg);
}

void StaticFallbackBackend::processAudio(const uint8_t* pcm, size_t len) {
    (void)pcm;
    (void)len;
    processText("");  // Same fallback for any audio input
}

void StaticFallbackBackend::setMessages(std::vector<std::string> messages) {
    if (!messages.empty()) {
        messages_ = std::move(messages);
        msgIndex_ = 0;
    }
}

const std::string& StaticFallbackBackend::pickMessage() const {
    const std::string& msg = messages_[msgIndex_];
    msgIndex_ = (msgIndex_ + 1) % messages_.size();
    return msg;
}
