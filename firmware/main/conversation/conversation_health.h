#pragma once

// ---------------------------------------------------------------------------
// ConversationHealth
// ---------------------------------------------------------------------------
// Central health state. Updated asynchronously by event callbacks.
// Backend selection reads from this struct only (no per-turn I/O).
// ---------------------------------------------------------------------------
struct ConversationHealth {
    // Online (Xiaozhi) --------------------------------------------------------
    bool networkConnected   = false;
    bool onlineReady        = false;   // Xiaozhi WebSocket connected + auth OK

    // Module LLM (local) ------------------------------------------------------
    bool localLLMConnected  = false;   // UART link is up
    bool localLLMReady      = false;   // StackFlow + all models loaded
    bool localPipelineReady = false;   // Whisper+Qwen+MeloTTS pipeline confirmed

    // Derived helpers ---------------------------------------------------------
    bool canUseOnline() const { return onlineReady; }

    bool canUseLocal() const { return localLLMConnected &&
                                      localLLMReady    &&
                                      localPipelineReady; }
};

// ---------------------------------------------------------------------------
// ModuleLLMState — internal state machine for the Module LLM client
// ---------------------------------------------------------------------------
enum class ModuleLLMState {
    NotConnected,
    Connected,
    Booting,
    ModelLoading,
    PipelineReady,
    Error,
};
