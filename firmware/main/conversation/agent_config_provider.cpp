#include "agent_config_provider.h"
#include <esp_log.h>

static const char* TAG = "AgentCfgProv";

AgentConfigProvider::AgentConfigProvider()  = default;
AgentConfigProvider::~AgentConfigProvider() = default;

void AgentConfigProvider::setEndpoint(const std::string&) {}

// Tenclass サーバーに personality を取得する公開 API は存在しない。
// fetch() は常に nullopt を返す。
// LocalOnly では NVS キャッシュがあれば再利用、なければ Qwen3 デフォルト動作。
std::optional<CachedAgentConfig> AgentConfigProvider::fetch() {
    ESP_LOGD(TAG, "fetch() disabled — no public API available");
    return std::nullopt;
}

std::optional<CachedAgentConfig> AgentConfigProvider::parse(const std::string&) {
    return std::nullopt;
}

bool AgentConfigProvider::isValid(const CachedAgentConfig& cfg) const {
    return !cfg.language.empty() || !cfg.character.empty();
}
