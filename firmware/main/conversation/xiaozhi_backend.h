#pragma once

#include "conversation_backend.h"

// ---------------------------------------------------------------------------
// XiaozhiBackend
// ---------------------------------------------------------------------------
// Thin wrapper around the existing Xiaozhi WebSocket pipeline.
// Existing StackChan code continues to own the actual Xiaozhi logic;
// this class routes ConversationManager calls into it.
// ---------------------------------------------------------------------------
class XiaozhiBackend final : public ConversationBackend {
public:
    XiaozhiBackend();
    ~XiaozhiBackend() override;

    BackendKind kind() const override { return BackendKind::XiaozhiOnline; }

    void start()     override;
    void stop()      override;
    void beginTurn() override;
    void endTurn()   override;

    void processText(const std::string& text) override;
    void processAudio(const uint8_t* pcm, size_t len) override;
};
