#pragma once

#include "conversation_backend.h"
#include "agent_config_store.h"
#include "module_llm_client.h"
#include <atomic>

#include <cstdint>
#include <memory>
#include <string>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <cJSON.h>

class ModuleLLMBackend final : public ConversationBackend {
public:
    explicit ModuleLLMBackend(std::shared_ptr<ModuleLLMClient> client);
    ~ModuleLLMBackend() override;

    BackendKind kind() const override { return BackendKind::ModuleLLM; }

    void start()     override;
    void stop()      override;
    void abortSpeaking() { abortRequested_.store(true); }  // 顔タッチ中断
    void beginTurn() override;
    void endTurn()   override;

    void processText(const std::string& text) override;
    void processAudio(const uint8_t* pcm, size_t len) override;

    void applyConfig(const CachedAgentConfig& cfg);

private:
    // UART polling task — reads Whisper ASR results and drives LLM
    void pollLoop();
    void onAsrResult(const std::string& text);

    // LLM → TTS pipeline
    void runLlmTts(const std::string& userText);
    void handleAbortRequest();
    void startLocalMouthAnimation(const char* reason);
    void stopLocalMouthAnimation(const char* reason);
    void resetVadTracking();
    void finishLocalTurn(const char* reason);
    void resumePausedUnitsForNextTurn();
    std::string nextRequestId(const char* prefix);
    void playback(const uint8_t* pcm, size_t len);

    std::shared_ptr<ModuleLLMClient> client_;
    CachedAgentConfig                config_;
    std::atomic<bool>                active_          {false};
    std::atomic<bool>                taskRunning_     {false};
    bool                             inThinkBlock_ = false;
    bool                             micMuted_         = false;
    bool                             ttsDispatched_    = false;
    bool                             localMouthAnimationActive_ = false;
    bool                             llmPausedForAbort_ = false;
    bool                             ttsPausedForAbort_ = false;
    std::atomic<bool>                abortRequested_   {false};  // 顔タッチ中断フラグ
    std::atomic<uint32_t>            requestSeq_        {0};
    int                              lastVadSpeech_    = -1;  // -1=未初期化, 0=silence, 1=speech
    int64_t                          lastVadSpeechStartMs_ = 0;
    int64_t                          lastVadSpeechEndMs_   = 0;
    int64_t                          lastAsrResultMs_      = 0;
    int64_t                          currentLlmStartedMs_  = 0;
    bool                             llmFirstChunkSeen_    = false;
    std::string                      pendingTts_;
    std::string                      thinkTagCarry_;
    std::string                      currentLlmRequestId_;
    std::string                      currentTtsRequestId_;
    TaskHandle_t                     pollTask_     = nullptr;
    SemaphoreHandle_t                pollTaskDone_ = nullptr;
};
