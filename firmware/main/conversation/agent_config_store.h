#pragma once

#include <optional>
#include <string>

// ---------------------------------------------------------------------------
// LocalTtsConfig (Section 12)
// ---------------------------------------------------------------------------
struct LocalTtsConfig {
    std::string modelId  = "melotts-ja-jp";
    std::string language = "ja_JP";
    float speed          = 1.0f;
    float pitch          = 1.0f;
};

// ---------------------------------------------------------------------------
// CachedAgentConfig (Section 12)
// ---------------------------------------------------------------------------
struct CachedAgentConfig {
    std::string language  = "ja";
    std::string character;
    std::string memory;
    LocalTtsConfig localTts;
};

// ---------------------------------------------------------------------------
// AgentConfigStore — NVS-backed persistence (Section 10.2 / 10.3)
// ---------------------------------------------------------------------------
class AgentConfigStore {
public:
    AgentConfigStore();
    ~AgentConfigStore();

    // Persist config to NVS.  Returns true on success.
    bool save(const CachedAgentConfig& cfg);

    // Load config from NVS.  Returns nullopt if not present or corrupt.
    std::optional<CachedAgentConfig> load();

    // Erase stored config.
    void clear();

private:
    static constexpr const char* kNvsNamespace = "agentcfg";
    static constexpr const char* kNvsKey       = "config_v1";
};
