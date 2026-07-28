#pragma once

#include "conversation_health.h"
#include "agent_config_store.h"

#include <cstdint>
#include <functional>
#include <string>

// Forward declaration
struct cJSON;

// ---------------------------------------------------------------------------
// ModuleLLMClient
// ---------------------------------------------------------------------------
// Manages the UART link to M5Stack Module LLM and the StackFlow JSON-RPC
// protocol for loading models, building the pipeline, and streaming
// Whisper → Qwen3 → MeloTTS inference.
//
// UART pins (CoreS3, Section 9.1):
//   RX = GPIO 18
//   TX = GPIO 17
//   Baud = 115200 (Module LLM default)
// ---------------------------------------------------------------------------

// Result types for async inference
struct AsrResult {
    bool        ok = false;
    std::string text;
};

struct LlmResult {
    bool        ok = false;
    std::string text;
    bool        done = false;   // last chunk of streamed response
};

struct TtsResult {
    bool     ok  = false;
    uint8_t* pcm = nullptr;   // caller does NOT free; valid until next call
    size_t   len = 0;
};

// ---------------------------------------------------------------------------

class ModuleLLMClient {
public:
    ModuleLLMClient();
    ~ModuleLLMClient();

    // Callbacks for async results
    using AsrCallback = std::function<void(const AsrResult&)>;
    using LlmCallback = std::function<void(const LlmResult&)>;
    using TtsCallback = std::function<void(const TtsResult&)>;

    // ------------------------------------------------------------------
    // Connection (startup sequence — Section 4.1)
    // ------------------------------------------------------------------

    // Open UART, verify Module LLM is responding.
    // Returns true if UART + StackFlow handshake succeed.
    bool connect();

    // Load Whisper + Qwen3 + MeloTTS and verify pipeline.
    // Blocking — call once at startup to avoid first-turn latency (Section 9.3).
    // Returns true if all models are loaded and pipeline is confirmed.
    bool loadModelsAndPipeline();

    // Apply AgentConfig settings (language, TTS params, system prompt)
    void applyConfig(const CachedAgentConfig& cfg);

    // LLM thinking モード設定（NVS に永続化）
    void setThinkingEnabled(bool enabled);
    bool isThinkingEnabled() const { return thinkingEnabled_; }

    // VAD 有効・無効設定（NVS に永続化）
    void setVadEnabled(bool enabled);
    bool isVadEnabled() const { return vadEnabled_; }

    // TTS 言語（0=ja 1=zh 2=en-us 3=en-default）
    uint8_t getTtsLang() const { return ttsLang_; }

    // ------------------------------------------------------------------
    // Per-turn inference
    // ------------------------------------------------------------------

    // Audio → Whisper ASR (async; result delivered via cb)
    void runAsr(const uint8_t* pcm, size_t len, AsrCallback cb);

    // Text → Qwen3 LLM (streaming; cb called per chunk, done=true on last)
    void runLlm(const std::string& prompt, LlmCallback cb);

    // Text → MeloTTS → PCM audio (async; cb called when audio ready)
    void runTts(const std::string& text, TtsCallback cb);

    // Cancel any in-progress inference on this turn
    void cancelTurn();

    // ------------------------------------------------------------------
    // State
    // ------------------------------------------------------------------
    ModuleLLMState state() const { return state_; }
    bool isReady() const { return state_ == ModuleLLMState::PipelineReady; }

    // Public so ModuleLLMBackend polling loop can read UART messages
    std::string stackflowReceive(int timeoutMs = 5000);
    bool        stackflowSend(const std::string& jsonMsg);

    // Send filtered text to MeloTTS manually
    bool sendToTts(const std::string& text, bool finish = false);
    bool sendToOpenJTalkTts(const std::string& requestId, const std::string& text);
    bool pauseWhisper();         // TTS再生中はASR/VAD PCM bridgeを停止
    bool resumeWhisper();        // TTS完了後にASR/VAD PCM bridgeを再開
    bool pauseLlm();             // 顔タッチ中断: 生成中のLLM unitを停止
    bool resumeLlm();            // 次ターン開始前にLLM unitを再開
    bool pauseTts();             // 顔タッチ中断: 再生中のMeloTTS unitを停止
    bool resumeTts();            // 次のTTS送信前にMeloTTS unitを再開
    bool stopOpenJTalkTts();      // 顔タッチ中断: Open JTalk/aplay を停止
    bool isOpenJTalkTtsReady() const { return openJTalkTtsReady_; }

    // Work ID accessors for backend use
    const std::string& llmWorkId()     const { return llmWorkId_; }
    const std::string& melottsWorkId() const { return melottsWorkId_; }

private:
    // Low-level StackFlow JSON-RPC helpers
    bool waitForAck(const std::string& method, int timeoutMs = 10000);
    // UART handle (platform-specific)
    int          uartFd_    = -1;
    ModuleLLMState state_   = ModuleLLMState::NotConnected;

    // Config applied via applyConfig()
    CachedAgentConfig config_;

    // StackFlow unit work IDs (set after setup)
    std::string audioWorkId_;
    std::string vadWorkId_;
    std::string whisperWorkId_;
    std::string llmWorkId_;
    std::string melottsWorkId_;
    bool openJTalkTtsReady_ = false;
    bool thinkingEnabled_ = false;
    bool vadEnabled_      = true;
    uint8_t ttsLang_      = 0;   // 0=ja 1=zh 2=en、NVS から復元

    // Send a StackFlow command and return response work_id (empty on error)
    std::string sfCommand(const std::string& reqId,
                          const std::string& workId,
                          const std::string& action,
                          cJSON*             dataObj,
                          int                timeoutMs = 10000);
    bool sendAction(const std::string& reqId,
                    const std::string& workId,
                    const char* action);
    bool sysBashExec(const std::string& reqId,
                     const std::string& command,
                     int timeoutMs = 0,
                     std::string* output = nullptr);
    bool setVadPcmBridgePaused(bool paused);
    bool applyModuleMicrophoneGain(const std::string& requestId);
    bool checkOpenJTalkTts();

    // UART configuration (Section 9.1)
    static constexpr int  kRxPin   = 18;
    static constexpr int  kTxPin   = 17;
    static constexpr int  kBaud    = 115200;
    static constexpr int  kUartNum = 2;   // UART2 on CoreS3
};
