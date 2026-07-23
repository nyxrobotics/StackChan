#pragma once

#include "agent_config_store.h"   // for CachedAgentConfig
#include <optional>
#include <string>

// ---------------------------------------------------------------------------
// AgentConfigProvider
// ---------------------------------------------------------------------------
// Fetches AgentConfig from the StackChan server REST endpoint.
// Fetch is attempted once during startup (Section 10.1).
// Validation rejects empty / obviously corrupt payloads (Section 10.2).
// ---------------------------------------------------------------------------
class AgentConfigProvider {
public:
    AgentConfigProvider();
    ~AgentConfigProvider();

    // Blocking HTTP GET to the StackChan server.
    // Returns nullopt on network error, timeout, or parse failure.
    std::optional<CachedAgentConfig> fetch();

    // Returns true if cfg contains the minimum required fields.
    bool isValid(const CachedAgentConfig& cfg) const;

    // Configure the endpoint (optional — defaults to firmware NVS setting)
    void setEndpoint(const std::string& url);

private:
    std::string endpoint_;

    // Parse JSON response body into CachedAgentConfig.
    // Returns nullopt on parse failure.
    std::optional<CachedAgentConfig> parse(const std::string& json);
};
