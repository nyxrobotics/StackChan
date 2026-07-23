#pragma once

#include "conversation_backend.h"
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// StaticFallbackBackend
// ---------------------------------------------------------------------------
// Last-resort backend: responds with a pre-baked local message when both
// Xiaozhi and Module LLM are unavailable.
// No network, no UART, no LLM — pure firmware response.
// ---------------------------------------------------------------------------
class StaticFallbackBackend final : public ConversationBackend {
public:
    StaticFallbackBackend();
    ~StaticFallbackBackend() override;

    BackendKind kind() const override { return BackendKind::StaticFallback; }

    void start()     override;
    void stop()      override;
    void beginTurn() override;
    void endTurn()   override;

    void processText(const std::string& text) override;
    void processAudio(const uint8_t* pcm, size_t len) override;

    // Optionally override the fallback message list at runtime
    void setMessages(std::vector<std::string> messages);

private:
    const std::string& pickMessage() const;

    std::vector<std::string> messages_;
    mutable size_t           msgIndex_ = 0;
};
