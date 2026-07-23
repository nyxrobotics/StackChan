#pragma once

#include <cstdint>
#include <functional>
#include <string>

// ---------------------------------------------------------------------------
// BackendKind
// ---------------------------------------------------------------------------
enum class BackendKind {
    XiaozhiOnline,
    ModuleLLM,
    StaticFallback,
};

// ---------------------------------------------------------------------------
// ConversationMode — selectable at runtime or via build config
// ---------------------------------------------------------------------------
enum class ConversationMode {
    Auto,        // Xiaozhi → ModuleLLM → StaticFallback
    OnlineOnly,  // Xiaozhi → StaticFallback
    LocalOnly,   // ModuleLLM → StaticFallback
};

// ---------------------------------------------------------------------------
// ConversationBackend — pure abstract interface
// All backends must implement every method.
// ---------------------------------------------------------------------------
class ConversationBackend {
public:
    virtual ~ConversationBackend() = default;

    // Identify which backend this is
    virtual BackendKind kind() const = 0;

    // Lifecycle ---------------------------------------------------------------
    // Called once when the backend becomes active.
    virtual void start() = 0;
    // Called once when the backend is deactivated (between turns).
    virtual void stop() = 0;

    // Turn lifecycle ----------------------------------------------------------
    // Called at the start of each conversation turn.
    virtual void beginTurn() = 0;
    // Called when the turn (ASR + LLM + TTS + playback) has fully completed.
    virtual void endTurn() = 0;

    // Input processing --------------------------------------------------------
    // Process pre-transcribed text (e.g. text-only input path).
    virtual void processText(const std::string& text) = 0;
    // Process raw PCM audio: audio → ASR → LLM → TTS → playback.
    virtual void processAudio(const uint8_t* pcm, size_t len) = 0;

    // Failure callback --------------------------------------------------------
    // The backend calls this when an unrecoverable error occurs mid-turn.
    // ConversationManager uses it to trigger StaticFallback for the remainder.
    using FailureCallback = std::function<void(BackendKind failed)>;
    virtual void setFailureCallback(FailureCallback cb) { onFailure_ = std::move(cb); }

protected:
    FailureCallback onFailure_;
};
