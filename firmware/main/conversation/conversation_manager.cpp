#include "conversation_manager.h"
#include "xiaozhi_backend.h"
#include "module_llm_backend.h"
#include "module_llm_client.h"
#include "static_fallback_backend.h"
#include "hal/hal_bridge_conv.h"

#include <esp_log.h>
#include <settings.h>   // xiaozhi-esp32 NVS wrapper

static const char* TAG = "ConvMgr";

static_assert(static_cast<int>(BackendKind::XiaozhiOnline) == hal_bridge::kBackendXiaozhiOnline);
static_assert(static_cast<int>(BackendKind::ModuleLLM) == hal_bridge::kBackendModuleLLM);
static_assert(static_cast<int>(BackendKind::StaticFallback) == hal_bridge::kBackendStatic);

// NVS namespace / key
static constexpr const char* kNvsNs  = "conv_mgr";
static constexpr const char* kNvsKey = "mode";
static constexpr const char* kModuleLlmNvsNs = "modllm_cfg";
static constexpr const char* kAutoPrewarmKey = "auto_prewarm";

static bool loadAutoPrewarmFromNvs()
{
    Settings s(kModuleLlmNvsNs, false);
    return s.GetInt(kAutoPrewarmKey, 0) != 0;
}

// ---------------------------------------------------------------------------
// NVS helpers
// ---------------------------------------------------------------------------

ConversationMode ConversationManager::loadModeFromNvs()
{
    Settings s(kNvsNs, false);
    int32_t v = s.GetInt(kNvsKey, static_cast<int32_t>(ConversationMode::Auto));
    switch (v) {
        case static_cast<int32_t>(ConversationMode::LocalOnly):   return ConversationMode::LocalOnly;
        case static_cast<int32_t>(ConversationMode::OnlineOnly):  return ConversationMode::OnlineOnly;
        default:                                                   return ConversationMode::Auto;
    }
}

void ConversationManager::saveModeToNvs(ConversationMode mode)
{
    Settings s(kNvsNs, true);
    s.SetInt(kNvsKey, static_cast<int32_t>(mode));
}

void ConversationManager::setMode(ConversationMode mode)
{
    if (mode_ == mode) return;
    ESP_LOGI(TAG, "setMode: %d -> %d", static_cast<int>(mode_), static_cast<int>(mode));
    mode_ = mode;
    saveModeToNvs(mode);
    // Re-evaluate backend immediately (no I/O)
    BackendKind chosen = selectBackendFast();
    activateBackend(chosen);

    if (mode == ConversationMode::OnlineOnly) {
        requestModuleStop("online-only mode selected");
    } else if (mode == ConversationMode::LocalOnly) {
        requestModuleRecovery("local-only mode selected");
    } else if (!health_.canUseOnline()) {
        requestModuleRecovery("Auto mode needs local fallback");
    } else if (autoPrewarmEnabled_) {
        requestModulePrewarm("Auto fast fallback selected");
    } else {
        requestModuleStop("Auto power-save selected");
    }
}

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

ConversationManager::ConversationManager(ConversationMode mode)
    : mode_(mode), autoPrewarmEnabled_(loadAutoPrewarmFromNvs())
{
    llmClient_ = std::make_shared<ModuleLLMClient>();

    xiaozhiBackend_       = std::make_unique<XiaozhiBackend>();
    moduleLLMBackend_     = std::make_unique<ModuleLLMBackend>(llmClient_);
    staticFallbackBackend_= std::make_unique<StaticFallbackBackend>();

    // Register failure callbacks — escalate to next-best backend
    auto failCb = [this](BackendKind failed) { onBackendFailure(failed); };
    xiaozhiBackend_->setFailureCallback(failCb);
    moduleLLMBackend_->setFailureCallback(failCb);
    staticFallbackBackend_->setFailureCallback(failCb);
}

ConversationManager::~ConversationManager() {
    stop();
}

// ---------------------------------------------------------------------------
// Startup sequence (Section 4.1)
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// initModuleLLM — Module LLM の接続・パイプライン構築
// LocalOnly 起動時 と Auto でネット失敗時に呼ぶ
// ---------------------------------------------------------------------------

void ConversationManager::initModuleLLM() {
    if (llmInitialized_) return;

    const ConversationMode mode = mode_;
    if (mode == ConversationMode::OnlineOnly ||
        (mode == ConversationMode::Auto && health_.canUseOnline() &&
         !autoPrewarmEnabled_)) {
        ESP_LOGI(TAG, "Module LLM initialization skipped by power policy");
        return;
    }

    localStopRequested_.store(false);

    health_.localLLMConnected  = false;
    health_.localLLMReady      = false;
    health_.localPipelineReady = false;

    bool llmConnected = llmClient_->connect();
    if (!llmConnected) {
        ESP_LOGI(TAG, "Module LLM not connected — local backend disabled");
        return;
    }

    ESP_LOGI(TAG, "Module LLM connected, loading models...");
    localRuntimeStopConfirmed_.store(false);
    bool pipelineOk = llmClient_->loadModelsAndPipeline();
    if (!pipelineOk) {
        ESP_LOGW(TAG, "Module LLM pipeline NOT ready");
        llmClient_->disconnect();
        return;
    }

    health_.localLLMConnected  = true;
    health_.localLLMReady      = true;
    health_.localPipelineReady = true;
    llmInitialized_ = true;
    localRecoveryRequested_.store(false);
    localPrewarmRequested_.store(false);

    ESP_LOGI(TAG, "Module LLM pipeline ready");
    // NVS から config を読んで適用（オンライン時に保存済みのものを使う）
    runAgentConfigSequence();
}

// ---------------------------------------------------------------------------

void ConversationManager::startRecoveryTask() {
    if (recoveryTask_ != nullptr) return;

    recoveryTaskDone_ = xSemaphoreCreateBinary();
    if (recoveryTaskDone_ == nullptr) {
        ESP_LOGE(TAG, "Module LLM recovery semaphore allocation failed");
        return;
    }

    recoveryTaskRunning_.store(true);
    BaseType_t created = xTaskCreate([](void* arg) {
        auto* manager = static_cast<ConversationManager*>(arg);
        manager->recoveryLoop();
        if (manager->recoveryTaskDone_ != nullptr) {
            xSemaphoreGive(manager->recoveryTaskDone_);
        }
        while (true) {
            vTaskSuspend(nullptr);
        }
    }, "modllm_recover", 16384, this, 4, &recoveryTask_);

    if (created != pdPASS) {
        recoveryTaskRunning_.store(false);
        recoveryTask_ = nullptr;
        vSemaphoreDelete(recoveryTaskDone_);
        recoveryTaskDone_ = nullptr;
        ESP_LOGE(TAG, "Module LLM recovery task creation failed");
        return;
    }

    if (localRecoveryRequested_.load() || localStopRequested_.load()) {
        xTaskNotifyGive(recoveryTask_);
    }
}

void ConversationManager::stopRecoveryTask() {
    if (recoveryTask_ == nullptr) return;

    recoveryTaskRunning_.store(false);
    localRecoveryRequested_.store(false);
    localPrewarmRequested_.store(false);
    localStopRequested_.store(false);
    xTaskNotifyGive(recoveryTask_);

    if (recoveryTaskDone_ != nullptr &&
        xSemaphoreTake(recoveryTaskDone_, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGW(TAG, "Waiting for the current Module LLM recovery operation to finish");
        xSemaphoreTake(recoveryTaskDone_, portMAX_DELAY);
    }

    vTaskDelete(recoveryTask_);
    recoveryTask_ = nullptr;
    if (recoveryTaskDone_ != nullptr) {
        vSemaphoreDelete(recoveryTaskDone_);
        recoveryTaskDone_ = nullptr;
    }
    localStopInProgress_.store(false);
}

void ConversationManager::requestModuleRecovery(const char* reason) {
    const ConversationMode mode = mode_;
    if (mode == ConversationMode::OnlineOnly) return;

    localStopRequested_.store(false);
    localPrewarmRequested_.store(false);
    if (llmInitialized_) return;

    const bool alreadyRequested = localRecoveryRequested_.exchange(true);
    if (!alreadyRequested) {
        ESP_LOGW(TAG, "Module LLM recovery requested: %s", reason ? reason : "unknown");
    }
    if (recoveryTask_ != nullptr) {
        xTaskNotifyGive(recoveryTask_);
    }
}

void ConversationManager::requestModulePrewarm(const char* reason) {
    if (mode_ != ConversationMode::Auto || !autoPrewarmEnabled_ || llmInitialized_) {
        return;
    }

    localStopRequested_.store(false);
    localPrewarmRequested_.store(true);
    const bool alreadyRequested = localRecoveryRequested_.exchange(true);
    if (!alreadyRequested) {
        ESP_LOGI(TAG, "Module LLM standby prewarm requested: %s", reason ? reason : "unknown");
    }
    if (recoveryTask_ != nullptr) {
        xTaskNotifyGive(recoveryTask_);
    }
}

void ConversationManager::requestModuleStop(const char* reason) {
    localRecoveryRequested_.store(false);
    localPrewarmRequested_.store(false);
    if (localStopInProgress_.load() ||
        (localRuntimeStopConfirmed_.load() && !llmInitialized_)) {
        return;
    }

    const bool alreadyRequested = localStopRequested_.exchange(true);
    if (!alreadyRequested) {
        ESP_LOGI(TAG, "Module LLM stop requested: %s", reason ? reason : "unknown");
    }
    if (recoveryTask_ != nullptr) {
        xTaskNotifyGive(recoveryTask_);
    }
}

bool ConversationManager::stopModuleRuntime() {
    ESP_LOGI(TAG, "Stopping Module LLM inference services");
    moduleLLMBackend_->stop();
    if (!moduleLLMBackend_->waitUntilIdle(12000)) {
        ESP_LOGE(TAG, "Module LLM UART reader did not become idle; stop deferred");
        return false;
    }

    bool stopped = false;
    for (int attempt = 1; attempt <= 3 && recoveryTaskRunning_.load(); ++attempt) {
        if (llmClient_->state() == ModuleLLMState::NotConnected &&
            !llmClient_->connect()) {
            ESP_LOGI(TAG, "Module LLM control plane unavailable for stop attempt %d", attempt);
        } else if (llmClient_->stopLocalRuntime()) {
            stopped = true;
            break;
        }

        llmClient_->disconnect();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    llmClient_->disconnect();
    llmInitialized_ = false;
    health_.localLLMConnected = false;
    health_.localLLMReady = false;
    health_.localPipelineReady = false;

    if (stopped) {
        localRuntimeStopConfirmed_.store(true);
        ESP_LOGI(TAG, "Module LLM inference services stopped");
    } else {
        localRuntimeStopConfirmed_.store(false);
        ESP_LOGW(TAG, "Unable to confirm Module LLM inference service stop");
    }
    return stopped;
}

bool ConversationManager::waitForRecoveryDelay(int delayMs) {
    TickType_t delayTicks = pdMS_TO_TICKS(delayMs);
    if (delayTicks == 0) delayTicks = 1;
    const TickType_t started = xTaskGetTickCount();

    while (recoveryTaskRunning_.load() && localRecoveryRequested_.load()) {
        const TickType_t elapsed = xTaskGetTickCount() - started;
        if (elapsed >= delayTicks) return true;
        ulTaskNotifyTake(pdTRUE, delayTicks - elapsed);
    }
    return false;
}

void ConversationManager::recoveryLoop() {
    static constexpr int kFirstRecoveryDelayMs = 100;
    static constexpr int kPrewarmDelayMs = 5000;
    static constexpr int kRetryDelayMs = 15000;

    while (recoveryTaskRunning_.load()) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!recoveryTaskRunning_.load()) break;

        if (localStopRequested_.load()) {
            localStopInProgress_.store(true);
            stopModuleRuntime();
            localStopRequested_.store(false);
            localStopInProgress_.store(false);
            continue;
        }

        int attempt = 0;
        while (recoveryTaskRunning_.load() && localRecoveryRequested_.load()) {
            const ConversationMode mode = mode_;
            if (mode == ConversationMode::OnlineOnly ||
                (mode == ConversationMode::Auto && health_.onlineReady &&
                 !localPrewarmRequested_.load())) {
                ESP_LOGI(TAG, "Module LLM recovery deferred while online backend is available");
                localRecoveryRequested_.store(false);
                break;
            }

            ++attempt;
            const int delayMs = attempt == 1
                ? (localPrewarmRequested_.load() ? kPrewarmDelayMs : kFirstRecoveryDelayMs)
                : kRetryDelayMs;
            ESP_LOGI(TAG, "Module LLM recovery attempt %d in %d ms", attempt, delayMs);
            if (!waitForRecoveryDelay(delayMs)) break;

            const ConversationMode currentMode = mode_;
            if (currentMode == ConversationMode::OnlineOnly ||
                (currentMode == ConversationMode::Auto && health_.onlineReady &&
                 !localPrewarmRequested_.load())) {
                localRecoveryRequested_.store(false);
                break;
            }

            moduleLLMBackend_->stop();
            if (!moduleLLMBackend_->waitUntilIdle(12000)) {
                ESP_LOGE(TAG, "Module LLM UART reader did not become idle; reconnect deferred");
                continue;
            }
            llmClient_->disconnect();
            llmInitialized_ = false;
            health_.localLLMConnected = false;
            health_.localLLMReady = false;
            health_.localPipelineReady = false;

            initModuleLLM();
            if (!recoveryTaskRunning_.load()) break;
            if (llmInitialized_) {
                ESP_LOGI(TAG, "Module LLM automatic recovery succeeded on attempt %d", attempt);
                localRecoveryRequested_.store(false);
                if (!localStopRequested_.load()) {
                    activateBackend(selectBackendFast());
                }
                break;
            }

            ESP_LOGW(TAG, "Module LLM recovery attempt %d failed", attempt);
            if (localPrewarmRequested_.load() && health_.onlineReady) {
                ESP_LOGW(TAG, "Module LLM standby prewarm failed; deferring retries until needed");
                localPrewarmRequested_.store(false);
                localRecoveryRequested_.store(false);
                break;
            }
        }
    }
}

// ---------------------------------------------------------------------------

void ConversationManager::start() {
    ESP_LOGI(TAG, "ConversationManager::start()");

    const ConversationMode mode = mode_;
    autoPrewarmEnabled_ = loadAutoPrewarmFromNvs();
    ESP_LOGI(TAG, "Auto local standby policy: %s",
             autoPrewarmEnabled_ ? "fast fallback" : "power save");
    startRecoveryTask();

    // LocalOnly needs the local pipeline before it can select a backend. Auto
    // waits for an online failure unless fast fallback was explicitly enabled.
    if (mode == ConversationMode::LocalOnly) {
        initModuleLLM();
    } else {
        ESP_LOGI(TAG, "mode=%d: deferring Module LLM initialization", static_cast<int>(mode));
    }

    // Step 4 — select initial backend and start it
    BackendKind initial = selectBackendFast();
    activateBackend(initial);

    if (mode == ConversationMode::LocalOnly && !llmInitialized_) {
        requestModuleRecovery("startup connection failed");
    } else if (mode == ConversationMode::OnlineOnly) {
        requestModuleStop("online-only startup");
    } else if (mode == ConversationMode::Auto && health_.canUseOnline()) {
        if (autoPrewarmEnabled_) {
            requestModulePrewarm("Auto startup fast fallback");
        } else {
            requestModuleStop("Auto startup power save");
        }
    }

    ESP_LOGI(TAG, "Initial backend: %d", static_cast<int>(initial));
}

void ConversationManager::stop() {
    stopRecoveryTask();
    if (activeBackend_) {
        activeBackend_->stop();
        activeBackend_ = nullptr;
    }
}

// ---------------------------------------------------------------------------
// AgentConfig sequence (Section 10)
// ---------------------------------------------------------------------------

void ConversationManager::runAgentConfigSequence() {
    if (health_.onlineReady) {
        // オンライン時: サーバーから取得して NVS に保存するだけ
        // Module LLM には適用しない（動いていないため）
        auto result = configProvider_.fetch();
        if (result.has_value() && configProvider_.isValid(*result)) {
            configStore_.save(*result);
            ESP_LOGI(TAG, "AgentConfig: fetched and saved to NVS");
        } else {
            ESP_LOGW(TAG, "AgentConfig: fetch failed, NVS cache unchanged");
        }
    } else {
        // LocalOnly / オフライン時: NVS から読んで Module LLM に適用
        auto cached = configStore_.load();
        if (cached.has_value()) {
            moduleLLMBackend_->applyConfig(*cached);
            ESP_LOGI(TAG, "AgentConfig: offline, loaded from NVS and applied");
        } else {
            ESP_LOGW(TAG, "AgentConfig: offline, no NVS cache — using defaults");
        }
    }
}

// ---------------------------------------------------------------------------
// Network / Xiaozhi event hooks (Section 7 + 13.1)
// ---------------------------------------------------------------------------

void ConversationManager::onNetworkConnected() {
    health_.networkConnected = true;
    ESP_LOGI(TAG, "Network connected");
}

void ConversationManager::onNetworkDisconnected() {
    health_.networkConnected = false;
    health_.onlineReady      = false;
    ESP_LOGW(TAG, "Network disconnected → onlineReady=false");
    // Auto モードでネット断 → Module LLM にフォールバック
    if (mode_ == ConversationMode::Auto) {
        requestModuleRecovery("network disconnected");
        activateBackend(selectBackendFast());
    }
}

void ConversationManager::onXiaozhiConnected() {
    health_.onlineReady = true;
    ESP_LOGI(TAG, "Xiaozhi connected → onlineReady=true");
    const ConversationMode mode = mode_;
    if (mode != ConversationMode::LocalOnly && !turnInProgress_) {
        activateBackend(selectBackendFast());
    }
    if (mode == ConversationMode::OnlineOnly) {
        requestModuleStop("online backend ready");
    } else if (mode == ConversationMode::Auto) {
        if (autoPrewarmEnabled_) {
            requestModulePrewarm("online backend ready");
        } else if (!turnInProgress_) {
            requestModuleStop("online backend recovered");
        }
    }
}

void ConversationManager::onXiaozhiDisconnected() {
    health_.onlineReady = false;
    ESP_LOGW(TAG, "Xiaozhi disconnected → onlineReady=false");
    // Auto モードで Xiaozhi 切断 → Module LLM にフォールバック
    if (mode_ == ConversationMode::Auto) {
        requestModuleRecovery("online backend disconnected");
        activateBackend(selectBackendFast());
    }
}

void ConversationManager::onXiaozhiError() {
    health_.onlineReady = false;
    ESP_LOGW(TAG, "Xiaozhi error → onlineReady=false");
    // Auto モードで Xiaozhi エラー → Module LLM にフォールバック
    if (mode_ == ConversationMode::Auto) {
        requestModuleRecovery("online backend error");
        activateBackend(selectBackendFast());
    }
}

// ---------------------------------------------------------------------------
// Turn lifecycle (Section 6)
// ---------------------------------------------------------------------------

void ConversationManager::onTurnStart() {
    // Select backend from cached state only — NO I/O (Section 2.2)
    BackendKind chosen = selectBackendFast();

    if (activeBackend_ == nullptr || chosen != activeBackend_->kind()) {
        activateBackend(chosen);
    }

    turnInProgress_ = true;
    activeBackend_->beginTurn();

    ESP_LOGD(TAG, "Turn started, backend=%d", static_cast<int>(chosen));
}

void ConversationManager::onTurnEnd() {
    if (activeBackend_) {
        activeBackend_->endTurn();
    }
    turnInProgress_ = false;

    BackendKind chosen = selectBackendFast();
    if (activeBackend_ == nullptr || activeBackend_->kind() != chosen) {
        activateBackend(chosen);
    }

    if (mode_ == ConversationMode::OnlineOnly ||
        (mode_ == ConversationMode::Auto && health_.canUseOnline() &&
         !autoPrewarmEnabled_)) {
        requestModuleStop("online turn boundary");
    }

    ESP_LOGD(TAG, "Turn ended");
}

// ---------------------------------------------------------------------------
// Input dispatch
// ---------------------------------------------------------------------------

void ConversationManager::processText(const std::string& text) {
    if (activeBackend_) {
        activeBackend_->processText(text);
    }
}

void ConversationManager::processAudio(const uint8_t* pcm, size_t len) {
    if (activeBackend_) {
        activeBackend_->processAudio(pcm, len);
    }
}

// ---------------------------------------------------------------------------
// Backend selection (Section 3.1 + 3.2) — NO I/O
// ---------------------------------------------------------------------------

BackendKind ConversationManager::selectBackendFast() const {
    switch (mode_) {
        case ConversationMode::OnlineOnly:
            // Xiaozhi → StaticFallback
            if (health_.canUseOnline()) return BackendKind::XiaozhiOnline;
            return BackendKind::StaticFallback;

        case ConversationMode::LocalOnly:
            // ModuleLLM → StaticFallback
            if (health_.canUseLocal()) return BackendKind::ModuleLLM;
            return BackendKind::StaticFallback;

        case ConversationMode::Auto:
        default:
            // Xiaozhi → ModuleLLM → StaticFallback (Section 3.1)
            if (health_.canUseOnline()) return BackendKind::XiaozhiOnline;
            if (health_.canUseLocal())  return BackendKind::ModuleLLM;
            return BackendKind::StaticFallback;
    }
}

// ---------------------------------------------------------------------------
// Backend activation (between turns only — enforced by caller)
// ---------------------------------------------------------------------------

void ConversationManager::activateBackend(BackendKind kind) {
    ConversationBackend* next = nullptr;

    switch (kind) {
        case BackendKind::XiaozhiOnline:
            next = xiaozhiBackend_.get();
            break;
        case BackendKind::ModuleLLM:
            next = moduleLLMBackend_.get();
            break;
        case BackendKind::StaticFallback:
        default:
            next = staticFallbackBackend_.get();
            break;
    }

    if (activeBackend_ == next) {
        hal_bridge::set_active_conversation_backend(static_cast<int>(kind));
        return;
    }

    if (activeBackend_) {
        activeBackend_->stop();
    }

    activeBackend_ = next;
    activeBackend_->start();
    hal_bridge::set_active_conversation_backend(static_cast<int>(kind));

    ESP_LOGI(TAG, "Activated backend: %d", static_cast<int>(kind));
}

// ---------------------------------------------------------------------------
// Mid-turn failure escalation (Section 8)
// ---------------------------------------------------------------------------

void ConversationManager::onBackendFailure(BackendKind failed) {
    ESP_LOGW(TAG, "Backend %d failed", static_cast<int>(failed));

    switch (failed) {
        case BackendKind::XiaozhiOnline:
            // Section 8.1 — retry same request via ModuleLLM
            health_.onlineReady = false;
            if (mode_ == ConversationMode::Auto) {
                requestModuleRecovery("online backend failure");
            }
            if (mode_ == ConversationMode::Auto && health_.canUseLocal()) {
                ESP_LOGI(TAG, "Xiaozhi failed → retrying with ModuleLLM");
                activateBackend(BackendKind::ModuleLLM);
            } else {
                ESP_LOGW(TAG, "Xiaozhi failed, selecting StaticFallback");
                activateBackend(BackendKind::StaticFallback);
            }
            if (mode_ == ConversationMode::OnlineOnly) {
                requestModuleStop("online backend failure in online-only mode");
            }
            break;

        case BackendKind::ModuleLLM:
            // Section 8.2
            health_.localLLMConnected  = false;
            health_.localLLMReady      = false;
            health_.localPipelineReady = false;
            llmInitialized_ = false;
            ESP_LOGW(TAG, "ModuleLLM failed");
            activateBackend(selectBackendFast());
            if (mode_ == ConversationMode::LocalOnly ||
                (mode_ == ConversationMode::Auto && !health_.canUseOnline())) {
                requestModuleRecovery("local backend failure");
            } else if (mode_ == ConversationMode::Auto && autoPrewarmEnabled_) {
                requestModulePrewarm("local standby failure");
            } else {
                requestModuleStop("local backend no longer needed");
            }
            break;

        case BackendKind::StaticFallback:
        default:
            // Nothing further to fall back to
            ESP_LOGE(TAG, "StaticFallback failed — no recovery possible");
            break;
    }
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

BackendKind ConversationManager::activeBackendKind() const {
    if (activeBackend_) return activeBackend_->kind();
    return BackendKind::StaticFallback;
}
