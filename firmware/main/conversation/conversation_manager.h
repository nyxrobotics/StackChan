#pragma once

#include "conversation_backend.h"
#include "conversation_health.h"
#include "agent_config_provider.h"
#include "agent_config_store.h"

#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <memory>

// Forward declarations
class XiaozhiBackend;
class ModuleLLMBackend;
class StaticFallbackBackend;
class ModuleLLMClient;

// ---------------------------------------------------------------------------
// ConversationManager
// ---------------------------------------------------------------------------
// Single owner of all backends and health state.
// Responsible for:
//   - startup sequence (Module LLM detection → AgentConfig → backend select)
//   - per-turn backend selection (cached, no I/O)
//   - delegating processText / processAudio to the active backend
//   - mid-turn failure escalation
//   - between-turn backend switching
// ---------------------------------------------------------------------------
class ConversationManager {
public:
    explicit ConversationManager(ConversationMode mode = ConversationMode::Auto);
    ~ConversationManager();

    // Called by AI.AGENT on startup
    void start();
    void stop();

    // ------------------------------------------------------------------
    // Network / Xiaozhi event hooks
    // Call these from Wi-Fi / Xiaozhi callbacks.
    // ------------------------------------------------------------------
    void onNetworkConnected();
    void onNetworkDisconnected();
    void onXiaozhiConnected();
    void onXiaozhiDisconnected();
    void onXiaozhiError();

    // ------------------------------------------------------------------
    // Turn lifecycle — called by AI.AGENT voice pipeline
    // ------------------------------------------------------------------
    void onTurnStart();
    void onTurnEnd();

    // ------------------------------------------------------------------
    // Input dispatch — forwarded to active backend
    // ------------------------------------------------------------------
    void processText(const std::string& text);
    void processAudio(const uint8_t* pcm, size_t len);

    // ------------------------------------------------------------------
    // Accessors
    // ------------------------------------------------------------------
    BackendKind activeBackendKind() const;
    const ConversationHealth& health() const { return health_; }

    // ------------------------------------------------------------------
    // Mode — can be changed at runtime; persisted to NVS
    // ------------------------------------------------------------------
    ConversationMode mode() const { return mode_; }
    void setMode(ConversationMode mode);

    // NVS helpers — called on startup and from ConversationModeWorker
    static ConversationMode loadModeFromNvs();
    static void             saveModeToNvs(ConversationMode mode);

    // Module LLM client accessor (for settings UI)
    ModuleLLMClient* getModuleLLMClient() const { return llmClient_.get(); }

    // Module LLM backend accessor (for abort from UI)
    ModuleLLMBackend* getModuleLLMBackend() const { return moduleLLMBackend_.get(); }

private:
    // Backend selection (no I/O, reads health_ only)
    BackendKind selectBackendFast() const;

    // Activate a backend (calls start() on new, stop() on old)
    void activateBackend(BackendKind kind);

    // Failure callback registered to each backend
    void onBackendFailure(BackendKind failed);

    // AgentConfig helpers
    void runAgentConfigSequence();

    // ---
    ConversationMode        mode_;
    ConversationHealth      health_;
    bool                    turnInProgress_  = false;
    bool                    llmInitialized_  = false;  // Module LLM 初期化済みフラグ

    // Backends (always allocated; only one is active at a time)
    std::unique_ptr<XiaozhiBackend>       xiaozhiBackend_;
    std::unique_ptr<ModuleLLMBackend>     moduleLLMBackend_;
    std::unique_ptr<StaticFallbackBackend> staticFallbackBackend_;

    ConversationBackend*    activeBackend_ = nullptr;

    // Module LLM UART client (shared with ModuleLLMBackend)
    std::shared_ptr<ModuleLLMClient> llmClient_;

    // Config subsystem
    AgentConfigProvider     configProvider_;
    AgentConfigStore        configStore_;

    // Module LLM initialization and recovery.
    void initModuleLLM();

    // Module LLM recovery runs outside backend callbacks because reconnecting
    // and loading models are blocking operations.
    void startRecoveryTask();
    void stopRecoveryTask();
    void requestModuleRecovery(const char* reason);
    void requestModulePrewarm(const char* reason);
    void requestModuleStop(const char* reason);
    bool stopModuleRuntime();
    bool waitForRecoveryDelay(int delayMs);
    void recoveryLoop();

    std::atomic<bool> localRecoveryRequested_ {false};
    std::atomic<bool> localPrewarmRequested_ {false};
    std::atomic<bool> localStopRequested_ {false};
    std::atomic<bool> localStopInProgress_ {false};
    std::atomic<bool> localRuntimeStopConfirmed_ {false};
    std::atomic<bool> recoveryTaskRunning_ {false};
    bool              autoPrewarmEnabled_ = false;
    TaskHandle_t      recoveryTask_ = nullptr;
    SemaphoreHandle_t recoveryTaskDone_ = nullptr;
};
