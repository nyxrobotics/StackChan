#include "conversation_manager.h"
#include "xiaozhi_backend.h"
#include "module_llm_backend.h"
#include "module_llm_client.h"
#include "static_fallback_backend.h"

#include <esp_log.h>
#include <settings.h>   // xiaozhi-esp32 NVS wrapper

static const char* TAG = "ConvMgr";

// NVS namespace / key
static constexpr const char* kNvsNs  = "conv_mgr";
static constexpr const char* kNvsKey = "mode";

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
}

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

ConversationManager::ConversationManager(ConversationMode mode)
    : mode_(mode)
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

    health_.localLLMConnected  = false;
    health_.localLLMReady      = false;
    health_.localPipelineReady = false;

    bool llmConnected = llmClient_->connect();
    if (!llmConnected) {
        ESP_LOGI(TAG, "Module LLM not connected — local backend disabled");
        return;
    }

    ESP_LOGI(TAG, "Module LLM connected, loading models...");
    bool pipelineOk = llmClient_->loadModelsAndPipeline();
    if (!pipelineOk) {
        ESP_LOGW(TAG, "Module LLM pipeline NOT ready");
        return;
    }

    health_.localLLMConnected  = true;
    health_.localLLMReady      = true;
    health_.localPipelineReady = true;
    llmInitialized_ = true;

    ESP_LOGI(TAG, "Module LLM pipeline ready");
    // NVS から config を読んで適用（オンライン時に保存済みのものを使う）
    runAgentConfigSequence();
}

// ---------------------------------------------------------------------------

void ConversationManager::start() {
    ESP_LOGI(TAG, "ConversationManager::start()");

    const ConversationMode mode = loadModeFromNvs();

    // LocalOnly: 起動時すぐに Module LLM を初期化
    // Auto: ネット失敗時に initModuleLLM() を呼ぶ（onXiaozhiError 等で）
    // OnlineOnly: Module LLM を使わない
    if (mode == ConversationMode::LocalOnly) {
        initModuleLLM();
    } else {
        ESP_LOGI(TAG, "mode=%d: skipping Module LLM init (will init on network failure if Auto)",
                 static_cast<int>(mode));
    }

    // Step 4 — select initial backend and start it
    BackendKind initial = selectBackendFast();
    activateBackend(initial);

    ESP_LOGI(TAG, "Initial backend: %d", static_cast<int>(initial));
}

void ConversationManager::stop() {
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
    if (loadModeFromNvs() == ConversationMode::Auto) {
        initModuleLLM();
        activateBackend(selectBackendFast());
    }
}

void ConversationManager::onXiaozhiConnected() {
    health_.onlineReady = true;
    ESP_LOGI(TAG, "Xiaozhi connected → onlineReady=true");
}

void ConversationManager::onXiaozhiDisconnected() {
    health_.onlineReady = false;
    ESP_LOGW(TAG, "Xiaozhi disconnected → onlineReady=false");
    // Auto モードで Xiaozhi 切断 → Module LLM にフォールバック
    if (loadModeFromNvs() == ConversationMode::Auto) {
        initModuleLLM();
        activateBackend(selectBackendFast());
    }
}

void ConversationManager::onXiaozhiError() {
    health_.onlineReady = false;
    ESP_LOGW(TAG, "Xiaozhi error → onlineReady=false");
    // Auto モードで Xiaozhi エラー → Module LLM にフォールバック
    if (loadModeFromNvs() == ConversationMode::Auto) {
        initModuleLLM();
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

    if (activeBackend_ && activeBackend_ != next) {
        activeBackend_->stop();
    }

    activeBackend_ = next;
    activeBackend_->start();

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
            if (health_.canUseLocal()) {
                ESP_LOGI(TAG, "Xiaozhi failed → retrying with ModuleLLM");
                activateBackend(BackendKind::ModuleLLM);
            } else {
                ESP_LOGW(TAG, "Xiaozhi failed, ModuleLLM unavailable → StaticFallback");
                activateBackend(BackendKind::StaticFallback);
            }
            break;

        case BackendKind::ModuleLLM:
            // Section 8.2
            health_.localLLMReady      = false;
            health_.localPipelineReady = false;
            ESP_LOGW(TAG, "ModuleLLM failed → StaticFallback");
            activateBackend(BackendKind::StaticFallback);
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
