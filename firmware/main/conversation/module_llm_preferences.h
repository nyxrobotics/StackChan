#pragma once

#include <cstdint>

namespace module_llm_preferences {

static constexpr const char* kNvsNamespace        = "modllm_cfg";
static constexpr const char* kThinkingKey         = "thinking";
static constexpr const char* kVadEnabledKey       = "vad_enabled";
static constexpr const char* kTtsLangKey          = "tts_lang";
static constexpr const char* kTtsVolumePercentKey = "tts_volume_pct";

// 100% preserves the legacy levels: StackFlow playVolume=0.15 and
// OpenJTalk gain=0 dB. Allow a conservative boost up to 200% (+6 dB).
static constexpr uint8_t kDefaultTtsVolumePercent = 100;
static constexpr uint8_t kMaxTtsVolumePercent     = 200;
static constexpr uint8_t kTtsVolumePercentStep    = 10;
static constexpr float   kLegacyMeloPlayVolume    = 0.15f;

constexpr uint8_t clampTtsVolumePercent(uint8_t percent)
{
    return percent > kMaxTtsVolumePercent ? kMaxTtsVolumePercent : percent;
}

constexpr float toMeloPlayVolume(uint8_t percent)
{
    return kLegacyMeloPlayVolume *
           static_cast<float>(clampTtsVolumePercent(percent)) / 100.0f;
}

}  // namespace module_llm_preferences
