#include "agent_config_store.h"

#include <esp_log.h>
#include <nvs.h>
#include <nvs_flash.h>

#include <cstring>

// Serialization: simple fixed-length binary blob for NVS storage.
// Replace with JSON (cJSON / nlohmann) if config grows more complex.

static const char* TAG = "AgentCfgStore";

// ---------------------------------------------------------------------------
// Blob layout (fixed, version-tagged)
// ---------------------------------------------------------------------------
static constexpr uint8_t kBlobVersion = 2;  // v2: speed/pitch デフォルト有効化

struct ConfigBlob {
    uint8_t  version;
    char     language[8];
    char     character[64];
    char     memory[256];
    // TTS
    char     ttsModelId[32];
    char     ttsLanguage[16];
    float    ttsSpeed;
    float    ttsPitch;
} __attribute__((packed));

// ---------------------------------------------------------------------------

AgentConfigStore::AgentConfigStore()  = default;
AgentConfigStore::~AgentConfigStore() = default;

bool AgentConfigStore::save(const CachedAgentConfig& cfg) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return false;
    }

    ConfigBlob blob{};
    blob.version = kBlobVersion;
    strncpy(blob.language,    cfg.language.c_str(),          sizeof(blob.language)    - 1);
    strncpy(blob.character,   cfg.character.c_str(),         sizeof(blob.character)   - 1);
    strncpy(blob.memory,      cfg.memory.c_str(),            sizeof(blob.memory)      - 1);
    strncpy(blob.ttsModelId,  cfg.localTts.modelId.c_str(),  sizeof(blob.ttsModelId)  - 1);
    strncpy(blob.ttsLanguage, cfg.localTts.language.c_str(), sizeof(blob.ttsLanguage) - 1);
    blob.ttsSpeed = cfg.localTts.speed;
    blob.ttsPitch = cfg.localTts.pitch;

    err = nvs_set_blob(handle, kNvsKey, &blob, sizeof(blob));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save failed: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "config saved to NVS: lang=%s character='%s' memory='%s' ttsModel=%s",
             cfg.language.c_str(), cfg.character.c_str(),
             cfg.memory.c_str(), cfg.localTts.modelId.c_str());
    return true;
}

std::optional<CachedAgentConfig> AgentConfigStore::load() {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "nvs_open (read) failed: %s", esp_err_to_name(err));
        return std::nullopt;
    }

    size_t    blobSize = sizeof(ConfigBlob);
    ConfigBlob blob{};
    err = nvs_get_blob(handle, kNvsKey, &blob, &blobSize);
    nvs_close(handle);

    if (err != ESP_OK || blobSize != sizeof(ConfigBlob)) {
        ESP_LOGD(TAG, "no valid blob in NVS");
        return std::nullopt;
    }

    if (blob.version != kBlobVersion) {
        ESP_LOGW(TAG, "NVS blob version mismatch (%u vs %u) — ignoring",
                 blob.version, kBlobVersion);
        return std::nullopt;
    }

    CachedAgentConfig cfg;
    cfg.language  = blob.language;
    cfg.character = blob.character;
    cfg.memory    = blob.memory;
    cfg.localTts.modelId  = blob.ttsModelId;
    cfg.localTts.language = blob.ttsLanguage;
    cfg.localTts.speed    = blob.ttsSpeed;
    cfg.localTts.pitch    = blob.ttsPitch;

    ESP_LOGI(TAG, "config loaded from NVS: lang=%s character='%s' memory='%s' ttsModel=%s",
             cfg.language.c_str(), cfg.character.c_str(),
             cfg.memory.c_str(), cfg.localTts.modelId.c_str());
    return cfg;
}

void AgentConfigStore::clear() {
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_erase_key(handle, kNvsKey);
        nvs_commit(handle);
        nvs_close(handle);
        ESP_LOGI(TAG, "NVS config erased");
    }
}
