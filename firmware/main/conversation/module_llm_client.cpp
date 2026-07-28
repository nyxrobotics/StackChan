#include "module_llm_client.h"

#include <driver/uart.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs_flash.h>
#include <nvs.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// StackFlow JSON-RPC protocol
//   Send:    {"request_id":"N","work_id":"unit","action":"action"[,"object":"...","data":{...}]}\n
//   Receive: {"created":...,"error":{"code":0,"message":""},"work_id":"unit.1001",...}\n
//   code==0  → success; work_id in response is the instance ID to reuse
// ---------------------------------------------------------------------------

static const char* TAG = "ModLLMClient";
static constexpr size_t kMaxStackflowFrameBytes = 16 * 1024;

// ---------------------------------------------------------------------------
// UART helpers
// ---------------------------------------------------------------------------

static bool uartInit(int uartNum, int txPin, int rxPin, int baud)
{
    uart_config_t cfg{};
    cfg.baud_rate  = baud;
    cfg.data_bits  = UART_DATA_8_BITS;
    cfg.parity     = UART_PARITY_DISABLE;
    cfg.stop_bits  = UART_STOP_BITS_1;
    cfg.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE;

    if (uart_param_config(static_cast<uart_port_t>(uartNum), &cfg) != ESP_OK) return false;
    if (uart_set_pin(static_cast<uart_port_t>(uartNum),
                     txPin, rxPin,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) return false;
    if (uart_driver_install(static_cast<uart_port_t>(uartNum),
                            4096, 4096, 0, nullptr, 0) != ESP_OK) return false;
    return true;
}

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

static constexpr const char* kLlmNvsNamespace = "modllm_cfg";
static constexpr const char* kThinkingKey     = "thinking";
static constexpr const char* kVadEnabledKey   = "vad_enabled";
static constexpr const char* kTtsLangKey      = "tts_lang";  // 0=ja 1=zh 2=en
static constexpr const char* kVadPcmBridgeCommand =
    "/opt/stackchan/stackchan_vad_pcm_bridge.py";
static constexpr const char* kVadPcmBridgePauseFile =
    "/run/stackchan-vad-pcm-bridge.paused";
static constexpr const char* kVadPcmWhisperInput = "whisper.vad.pcm.base64";
static constexpr double kVadSpeechThreshold = 0.10;
static constexpr double kVadMinSilenceSeconds = 1.0;
static constexpr double kWhisperRawChunkSeconds = 5.0;
static constexpr const char* kCaptureGainReadyMarker =
    "STACKCHAN_CAPTURE_GAIN_READY";
static constexpr const char* kCaptureGainCommand =
    "amixer -q -c 0 sset 'RX LEFT ANA GAIN' 54 && "
    "amixer -q -c 0 sset 'RX RIGHT ANA GAIN' 54 && "
    "amixer -q -c 0 sset 'RX LEFT DIG GAIN' 40 && "
    "amixer -q -c 0 sset 'RX RIGHT DIG GAIN' 40 && "
    "echo STACKCHAN_CAPTURE_GAIN_READY";

static std::string runtimeRequestId(const char* prefix)
{
    return std::string(prefix) + std::to_string(esp_timer_get_time());
}

static std::string shellQuote(const std::string& value)
{
    std::string quoted;
    quoted.reserve(value.size() + 2);
    quoted += '\'';
    for (char ch : value) {
        if (ch == '\'') {
            quoted += "'\\''";
        } else {
            quoted += ch;
        }
    }
    quoted += '\'';
    return quoted;
}

ModuleLLMClient::ModuleLLMClient()
{
    nvs_handle_t h;
    if (nvs_open(kLlmNvsNamespace, NVS_READONLY, &h) == ESP_OK) {
        uint8_t v = 0;
        if (nvs_get_u8(h, kThinkingKey, &v) == ESP_OK) thinkingEnabled_ = (v != 0);
        uint8_t vad = 1;
        if (nvs_get_u8(h, kVadEnabledKey, &vad) == ESP_OK) vadEnabled_ = (vad != 0);
        uint8_t lang = 0;
        if (nvs_get_u8(h, kTtsLangKey, &lang) == ESP_OK) ttsLang_ = lang;
        nvs_close(h);
    }

    ESP_LOGI(TAG, "thinkingEnabled=%d vadEnabled=%d ttsLang=%d (from NVS)",
             thinkingEnabled_, vadEnabled_, (int)ttsLang_);
}

void ModuleLLMClient::setThinkingEnabled(bool enabled)
{
    thinkingEnabled_ = enabled;
    nvs_handle_t h;
    if (nvs_open(kLlmNvsNamespace, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, kThinkingKey, enabled ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "thinkingEnabled set to %d", enabled);
}

void ModuleLLMClient::setVadEnabled(bool enabled)
{
    vadEnabled_ = enabled;
    nvs_handle_t h;
    if (nvs_open(kLlmNvsNamespace, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, kVadEnabledKey, enabled ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "vadEnabled set to %d", enabled);
}

ModuleLLMClient::~ModuleLLMClient() {
    disconnect();
}

void ModuleLLMClient::disconnect()
{
    if (uartFd_ >= 0) {
        uart_driver_delete(static_cast<uart_port_t>(kUartNum));
        uartFd_ = -1;
    }

    state_ = ModuleLLMState::NotConnected;
    rxFrameBuffer_.clear();
    rxFrameDepth_ = 0;
    rxFrameStarted_ = false;
    rxFrameInString_ = false;
    rxFrameEscape_ = false;

    audioWorkId_.clear();
    vadWorkId_.clear();
    whisperWorkId_.clear();
    llmWorkId_.clear();
    melottsWorkId_.clear();
    openJTalkTtsReady_ = false;
}

// ---------------------------------------------------------------------------
// StackFlow helpers
// ---------------------------------------------------------------------------

bool ModuleLLMClient::stackflowSend(const std::string& jsonMsg)
{
    if (uartFd_ < 0) return false;
    std::string line = jsonMsg + "\n";
    int written = uart_write_bytes(static_cast<uart_port_t>(kUartNum),
                                   line.c_str(), line.size());
    return written == static_cast<int>(line.size());
}

bool ModuleLLMClient::sendHealthPing(const std::string& requestId)
{
    if (requestId.empty()) return false;
    return sendAction(requestId, "sys", "ping");
}

bool ModuleLLMClient::sendAction(const std::string& reqId,
                                 const std::string& workId,
                                 const char* action)
{
    if (workId.empty() || action == nullptr) return false;

    cJSON* msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "request_id", reqId.c_str());
    cJSON_AddStringToObject(msg, "work_id",    workId.c_str());
    cJSON_AddStringToObject(msg, "action",     action);
    char* s = cJSON_PrintUnformatted(msg);
    bool ok = s && stackflowSend(s);
    if (s) free(s);
    cJSON_Delete(msg);
    return ok;
}

bool ModuleLLMClient::sysBashExec(const std::string& reqId,
                                  const std::string& command,
                                  int timeoutMs,
                                  std::string* output)
{
    if (command.empty()) return false;

    cJSON* msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "request_id", reqId.c_str());
    cJSON_AddStringToObject(msg, "work_id",    "sys");
    cJSON_AddStringToObject(msg, "action",     "bashexec");
    cJSON_AddStringToObject(msg, "object",     "sys.bashexec");
    cJSON_AddStringToObject(msg, "data",       command.c_str());

    char* s = cJSON_PrintUnformatted(msg);
    bool sent = s && stackflowSend(s);
    if (s) free(s);
    cJSON_Delete(msg);

    if (!sent || timeoutMs <= 0) {
        return sent;
    }

    int64_t deadline = esp_timer_get_time() / 1000 + timeoutMs;
    while (esp_timer_get_time() / 1000 < deadline) {
        int remaining = static_cast<int>(deadline - esp_timer_get_time() / 1000);
        if (remaining < 1) break;

        std::string resp = stackflowReceive(remaining > 500 ? 500 : remaining);
        if (resp.empty()) continue;

        cJSON* root = cJSON_Parse(resp.c_str());
        if (!root) continue;

        cJSON* rid = cJSON_GetObjectItemCaseSensitive(root, "request_id");
        if (!cJSON_IsString(rid) || reqId != rid->valuestring) {
            cJSON_Delete(root);
            continue;
        }

        bool ok = false;
        cJSON* errObj = cJSON_GetObjectItemCaseSensitive(root, "error");
        if (cJSON_IsObject(errObj)) {
            cJSON* code = cJSON_GetObjectItemCaseSensitive(errObj, "code");
            ok = cJSON_IsNumber(code) && code->valueint == 0;
        }

        if (output) {
            cJSON* data = cJSON_GetObjectItemCaseSensitive(root, "data");
            if (cJSON_IsString(data)) {
                *output = data->valuestring;
            } else {
                *output = resp;
            }
        }

        cJSON_Delete(root);
        return ok;
    }

    ESP_LOGW(TAG, "sys.bashexec timeout: %s", command.c_str());
    return false;
}

bool ModuleLLMClient::setVadPcmBridgePaused(bool paused)
{
    const char* expected = paused
        ? "STACKCHAN_VAD_PCM_BRIDGE_PAUSED"
        : "STACKCHAN_VAD_PCM_BRIDGE_RUNNING";
    const std::string command = std::string(kVadPcmBridgeCommand) +
        (paused ? " --pause && test -f " : " --resume && test ! -f ") +
        kVadPcmBridgePauseFile + " && echo " + expected;

    for (int attempt = 1; attempt <= 2; ++attempt) {
        std::string output;
        const std::string requestId = runtimeRequestId(
            paused ? "vad_bridge_pause_" : "vad_bridge_resume_");
        if (sysBashExec(requestId, command, 5000, &output) &&
            output.find(expected) != std::string::npos) {
            ESP_LOGI(TAG, "VAD PCM bridge %s confirmed",
                     paused ? "pause" : "resume");
            return true;
        }
        ESP_LOGW(TAG, "VAD PCM bridge %s attempt %d failed: %s",
                 paused ? "pause" : "resume", attempt, output.c_str());
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return false;
}

bool ModuleLLMClient::checkOpenJTalkTts()
{
    std::string output;
    bool ok = sysBashExec("ojt_check", "/opt/stackchan/openjtalk_tts.sh --check", 15000, &output);
    if (!ok || output.find("STACKCHAN_OPENJTALK_READY") == std::string::npos) {
        ESP_LOGW(TAG, "OpenJTalk TTS unavailable: %s", output.c_str());
        return false;
    }

    ESP_LOGI(TAG, "OpenJTalk TTS ready: %s", output.c_str());
    return true;
}

bool ModuleLLMClient::applyModuleMicrophoneGain(const std::string& requestId)
{
    std::string output;
    const bool ready = sysBashExec(
        requestId, kCaptureGainCommand, 10000, &output);
    if (!ready || output.find(kCaptureGainReadyMarker) == std::string::npos) {
        ESP_LOGE(TAG, "module microphone gain setup failed: %s", output.c_str());
        return false;
    }

    ESP_LOGI(TAG, "module microphone gain ready: RX analog=27.0 dB digital=20.0 dB");
    return true;
}

std::string ModuleLLMClient::stackflowReceive(int timeoutMs)
{
    if (timeoutMs <= 0) return {};

    const TickType_t startTick = xTaskGetTickCount();
    TickType_t timeoutTicks = pdMS_TO_TICKS(timeoutMs);
    if (timeoutTicks == 0) timeoutTicks = 1;

    TickType_t readWaitTicks = pdMS_TO_TICKS(10);
    if (readWaitTicks == 0) readWaitTicks = 1;

    while (true) {
        // Unsigned subtraction keeps this comparison safe across tick wrap.
        const TickType_t elapsed = xTaskGetTickCount() - startTick;
        if (elapsed >= timeoutTicks) break;

        const TickType_t remaining = timeoutTicks - elapsed;
        const TickType_t waitTicks =
            remaining < readWaitTicks ? remaining : readWaitTicks;

        uint8_t ch;
        int n = uart_read_bytes(static_cast<uart_port_t>(kUartNum),
                                &ch, 1, waitTicks);
        if (n <= 0) continue;

        // Ignore boot logs and other UART noise before the JSON object.
        if (!rxFrameStarted_) {
            if (ch != '{') continue;

            rxFrameStarted_ = true;
            rxFrameDepth_ = 1;
            rxFrameInString_ = false;
            rxFrameEscape_ = false;
            rxFrameBuffer_.clear();
            rxFrameBuffer_ += '{';
            continue;
        }

        rxFrameBuffer_ += static_cast<char>(ch);
        if (rxFrameBuffer_.size() > kMaxStackflowFrameBytes) {
            ESP_LOGW(TAG, "discarding oversized StackFlow frame (> %u bytes)",
                     static_cast<unsigned>(kMaxStackflowFrameBytes));
            rxFrameBuffer_.clear();
            rxFrameDepth_ = 0;
            rxFrameStarted_ = false;
            rxFrameInString_ = false;
            rxFrameEscape_ = false;
            continue;
        }

        // 文字列リテラル内のエスケープ処理
        if (rxFrameEscape_) { rxFrameEscape_ = false; continue; }
        if (ch == '\\' && rxFrameInString_) { rxFrameEscape_ = true; continue; }
        if (ch == '"') { rxFrameInString_ = !rxFrameInString_; }
        if (rxFrameInString_) continue;

        if (ch == '{') { rxFrameDepth_++; }
        else if (ch == '}') {
            rxFrameDepth_--;
            if (rxFrameDepth_ == 0) {
                std::string result;
                result.swap(rxFrameBuffer_);
                rxFrameStarted_ = false;
                rxFrameInString_ = false;
                rxFrameEscape_ = false;
                return result;
            }
        }
    }
    return {};
}

// Send a StackFlow command and return the work_id from the response.
// Returns empty string on failure.
std::string ModuleLLMClient::sfCommand(const std::string& reqId,
                                        const std::string& workId,
                                        const std::string& action,
                                        cJSON*             dataObj,   // may be nullptr
                                        int                timeoutMs)
{
    cJSON* msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "request_id", reqId.c_str());
    cJSON_AddStringToObject(msg, "work_id",    workId.c_str());
    cJSON_AddStringToObject(msg, "action",     action.c_str());
    // Add object field: "work_id.action" (e.g. "whisper.setup")
    std::string objField = workId + "." + action;
    cJSON_AddStringToObject(msg, "object", objField.c_str());
    if (dataObj) {
        cJSON_AddItemToObject(msg, "data", dataObj);
    }

    char* s = cJSON_PrintUnformatted(msg);
    bool sent = s && stackflowSend(s);
    if (s) free(s);
    // dataObj ownership transferred to msg
    cJSON_Delete(msg);

    if (!sent) {
        ESP_LOGE(TAG, "sfCommand(%s.%s): send failed", workId.c_str(), action.c_str());
        return "";
    }

    int ignored = 0;
    int64_t deadline = esp_timer_get_time() / 1000 + timeoutMs;
    while (esp_timer_get_time() / 1000 < deadline) {
        int remaining = static_cast<int>(deadline - esp_timer_get_time() / 1000);
        if (remaining < 1) break;

        std::string resp = stackflowReceive(remaining > 500 ? 500 : remaining);
        if (resp.empty()) continue;

        cJSON* root = cJSON_Parse(resp.c_str());
        if (!root) {
            ESP_LOGW(TAG, "sfCommand(%s.%s): non-json rx: %.200s",
                     workId.c_str(), action.c_str(), resp.c_str());
            continue;
        }

        // Check request_id matches
        cJSON* rid = cJSON_GetObjectItemCaseSensitive(root, "request_id");
        bool matches = false;
        char ridBuf[16] = {};
        const char* ridText = "<none>";
        if (cJSON_IsString(rid)) {
            ridText = rid->valuestring;
            matches = reqId == rid->valuestring;
        } else if (cJSON_IsNumber(rid)) {
            snprintf(ridBuf, sizeof(ridBuf), "%d", rid->valueint);
            ridText = ridBuf;
            matches = reqId == ridBuf;
        }
        if (!matches) {
            cJSON* wid = cJSON_GetObjectItemCaseSensitive(root, "work_id");
            const char* widText = cJSON_IsString(wid) ? wid->valuestring : "<none>";
            if (++ignored <= 5) {
                ESP_LOGI(TAG, "sfCommand(%s.%s): ignoring rx request_id=%s work_id=%s",
                         workId.c_str(), action.c_str(), ridText, widText);
            }
            cJSON_Delete(root);
            continue;
        }

        // Check error code
        cJSON* errObj = cJSON_GetObjectItemCaseSensitive(root, "error");
        bool ok = false;
        if (cJSON_IsObject(errObj)) {
            cJSON* code = cJSON_GetObjectItemCaseSensitive(errObj, "code");
            ok = cJSON_IsNumber(code) && (code->valueint == 0);
        }

        std::string retWorkId;
        if (ok) {
            cJSON* wid = cJSON_GetObjectItemCaseSensitive(root, "work_id");
            if (cJSON_IsString(wid)) retWorkId = wid->valuestring;
        } else {
            ESP_LOGW(TAG, "sfCommand(%s.%s): error — %s",
                     workId.c_str(), action.c_str(), resp.c_str());
        }

        cJSON_Delete(root);
        return retWorkId;
    }

    ESP_LOGW(TAG, "sfCommand(%s.%s): timeout after %d ms",
             workId.c_str(), action.c_str(), timeoutMs);
    return "";
}

// ---------------------------------------------------------------------------
// waitForAck — used after ping (no data payload)
// ---------------------------------------------------------------------------

bool ModuleLLMClient::waitForAck(const std::string& requestId,
                                 const char* label,
                                 int timeoutMs)
{
    const int64_t deadline = esp_timer_get_time() / 1000 + timeoutMs;
    while (esp_timer_get_time() / 1000 < deadline) {
        const int remaining = static_cast<int>(deadline - esp_timer_get_time() / 1000);
        if (remaining < 1) break;

        std::string resp = stackflowReceive(remaining > 500 ? 500 : remaining);
        if (resp.empty()) continue;

        cJSON* root = cJSON_Parse(resp.c_str());
        if (!root) {
            ESP_LOGW(TAG, "waitForAck(%s): parse error: %.200s",
                     label, resp.c_str());
            continue;
        }

        cJSON* rid = cJSON_GetObjectItemCaseSensitive(root, "request_id");
        const bool matches = cJSON_IsString(rid) && requestId == rid->valuestring;
        if (!matches) {
            cJSON_Delete(root);
            continue;
        }

        bool ok = false;
        cJSON* errObj = cJSON_GetObjectItemCaseSensitive(root, "error");
        if (cJSON_IsObject(errObj)) {
            cJSON* code = cJSON_GetObjectItemCaseSensitive(errObj, "code");
            ok = cJSON_IsNumber(code) && code->valueint == 0;
        }

        if (!ok) {
            ESP_LOGW(TAG, "waitForAck(%s): not ok — %s", label, resp.c_str());
        }

        cJSON_Delete(root);
        return ok;
    }

    ESP_LOGW(TAG, "waitForAck(%s): timeout", label);
    return false;
}

// ---------------------------------------------------------------------------
// connect() — UART open + StackFlow ping
// ---------------------------------------------------------------------------

bool ModuleLLMClient::connect()
{
    if (uartFd_ >= 0) {
        ESP_LOGW(TAG, "discarding stale UART connection before handshake");
        disconnect();
    }

    if (!uartInit(kUartNum, kTxPin, kRxPin, kBaud)) {
        ESP_LOGE(TAG, "UART init failed");
        disconnect();
        return false;
    }
    uartFd_ = kUartNum;
    uart_flush_input(static_cast<uart_port_t>(kUartNum));

    // Send ping — StackFlow format
    const std::string requestId = runtimeRequestId("ping_");
    cJSON* ping = cJSON_CreateObject();
    cJSON_AddStringToObject(ping, "request_id", requestId.c_str());
    cJSON_AddStringToObject(ping, "work_id",    "sys");
    cJSON_AddStringToObject(ping, "action",     "ping");
    char* pingStr = cJSON_PrintUnformatted(ping);
    bool sent = pingStr != nullptr && stackflowSend(pingStr);
    if (pingStr) free(pingStr);
    cJSON_Delete(ping);

    if (!sent) {
        ESP_LOGE(TAG, "ping send failed");
        disconnect();
        return false;
    }

    bool ack = waitForAck(requestId, "sys.ping", 3000);
    if (!ack) {
        ESP_LOGW(TAG, "Module LLM did not respond to ping");
        disconnect();
        return false;
    }

    state_ = ModuleLLMState::Connected;
    ESP_LOGI(TAG, "Module LLM connected");
    return true;
}

// ---------------------------------------------------------------------------
// loadModelsAndPipeline()
// Setup order:
//   1. release existing StackChan tasks without restarting services
//   2. audio.setup
//   3. vad.setup      (Silero VAD, input: sys.pcm)
//   4. whisper.setup  (ASR input: VAD-delimited PCM bridge when VAD is enabled)
//   5. llm.setup      (Qwen3, UART input)
//   6. melotts.setup  (MeloTTS ja-JP, input from LLM)
// ---------------------------------------------------------------------------

bool ModuleLLMClient::loadModelsAndPipeline()
{
    state_ = ModuleLLMState::ModelLoading;
    ESP_LOGI(TAG, "Setting up StackFlow units...");

    const bool bridgePaused = setVadPcmBridgePaused(true);
    if (!bridgePaused) {
        if (vadEnabled_) {
            ESP_LOGE(TAG, "VAD PCM bridge setup pause failed");
            state_ = ModuleLLMState::Error;
            return false;
        }
        ESP_LOGW(TAG, "VAD PCM bridge setup pause unavailable while VAD is disabled");
    }

    auto drainStackflowInput = [this](const char* label, int totalMs) {
        const int64_t deadline = esp_timer_get_time() / 1000 + totalMs;
        int drained = 0;
        while (esp_timer_get_time() / 1000 < deadline) {
            std::string resp = stackflowReceive(50);
            if (!resp.empty()) {
                ++drained;
                continue;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (drained > 0) {
            ESP_LOGI(TAG, "%s: drained %d stale StackFlow frame(s)", label, drained);
        }
    };

    auto waitForResponse = [this](const std::string& requestId, int timeoutMs) -> cJSON* {
        const int64_t deadline = esp_timer_get_time() / 1000 + timeoutMs;
        while (esp_timer_get_time() / 1000 < deadline) {
            std::string resp = stackflowReceive(500);
            if (resp.empty()) continue;

            cJSON* root = cJSON_Parse(resp.c_str());
            if (!root) continue;

            cJSON* rid = cJSON_GetObjectItemCaseSensitive(root, "request_id");
            bool matches = cJSON_IsString(rid) && requestId == rid->valuestring;
            if (!matches && cJSON_IsNumber(rid)) {
                matches = requestId == std::to_string(rid->valueint);
            }
            if (matches) return root;
            cJSON_Delete(root);
        }
        return nullptr;
    };

    auto responseSucceeded = [](cJSON* root) {
        if (!root) return false;
        cJSON* error = cJSON_GetObjectItemCaseSensitive(root, "error");
        cJSON* code = cJSON_IsObject(error)
            ? cJSON_GetObjectItemCaseSensitive(error, "code")
            : nullptr;
        return cJSON_IsNumber(code) && code->valueint == 0;
    };

    int cleanupSequence = 0;
    auto releaseTasks = [this, &cleanupSequence, &waitForResponse,
                         &responseSucceeded](const char* unit) -> bool {
        static constexpr int kCleanupResponseTimeoutMs = 15000;
        const std::string requestId = "list" + std::to_string(cleanupSequence++);
        cJSON* msg = cJSON_CreateObject();
        cJSON_AddStringToObject(msg, "request_id", requestId.c_str());
        cJSON_AddStringToObject(msg, "work_id", unit);
        // StackFlow returns <unit>.tasklist when taskinfo targets a base unit.
        cJSON_AddStringToObject(msg, "action", "taskinfo");
        char* encoded = cJSON_PrintUnformatted(msg);
        const bool sent = encoded && stackflowSend(encoded);
        if (encoded) free(encoded);
        cJSON_Delete(msg);
        if (!sent) {
            ESP_LOGE(TAG, "%s task list send failed", unit);
            return false;
        }

        cJSON* response = waitForResponse(requestId, kCleanupResponseTimeoutMs);
        cJSON* data = response
            ? cJSON_GetObjectItemCaseSensitive(response, "data")
            : nullptr;
        if (!responseSucceeded(response) || !cJSON_IsArray(data)) {
            char* dump = response ? cJSON_PrintUnformatted(response) : nullptr;
            ESP_LOGE(TAG, "%s task list failed: %s", unit, dump ? dump : "timeout");
            if (dump) free(dump);
            if (response) cJSON_Delete(response);
            return false;
        }

        std::vector<std::string> taskIds;
        cJSON* item = nullptr;
        cJSON_ArrayForEach(item, data) {
            if (cJSON_IsString(item) && item->valuestring && item->valuestring[0] != '\0') {
                taskIds.emplace_back(item->valuestring);
            }
        }
        cJSON_Delete(response);

        for (const std::string& taskId : taskIds) {
            const std::string exitRequest =
                "exit" + std::to_string(cleanupSequence++);
            if (!sendAction(exitRequest, taskId, "exit")) {
                ESP_LOGE(TAG, "%s exit send failed", taskId.c_str());
                return false;
            }

            cJSON* exitResponse = waitForResponse(exitRequest, kCleanupResponseTimeoutMs);
            const bool exited = responseSucceeded(exitResponse);
            if (!exited) {
                char* dump = exitResponse ? cJSON_PrintUnformatted(exitResponse) : nullptr;
                ESP_LOGE(TAG, "%s exit failed: %s", taskId.c_str(), dump ? dump : "timeout");
                if (dump) free(dump);
            }
            if (exitResponse) cJSON_Delete(exitResponse);
            if (!exited) return false;
        }

        ESP_LOGI(TAG, "%s cleanup: released %u task(s)", unit,
                 static_cast<unsigned>(taskIds.size()));
        return true;
    };

    // llm-audio 1.9 does not recover its capture device after sys.reset
    // restarts the service. Release only the model tasks and keep the Audio
    // service and driver alive across Core reboots.
    const char* cleanupUnits[] = {"melotts", "llm", "whisper", "vad"};
    for (const char* unit : cleanupUnits) {
        if (!releaseTasks(unit)) {
            state_ = ModuleLLMState::Error;
            return false;
        }
    }
    drainStackflowInput("post task cleanup", 250);

    auto waitForPcmFrame = [this]() {
        std::string output;
        const bool ok = sysBashExec(
            "pcm_ready",
            "/opt/stackchan/stackflow_pcm_ready.py --timeout 10",
            15000,
            &output);
        if (!ok || output.find("STACKCHAN_PCM_READY") == std::string::npos) {
            ESP_LOGE(TAG, "sys.pcm did not become ready: %s", output.c_str());
            return false;
        }
        ESP_LOGI(TAG, "sys.pcm ready: %s", output.c_str());
        return true;
    };

    // 2. Audio setup
    {
        ESP_LOGI(TAG, "audio.setup starting");
        cJSON* data = cJSON_CreateObject();
        cJSON_AddNumberToObject(data, "capcard",   0);
        cJSON_AddNumberToObject(data, "capdevice", 0);
        cJSON_AddNumberToObject(data, "capVolume", 10.0);
        cJSON_AddNumberToObject(data, "playcard",  0);
        cJSON_AddNumberToObject(data, "playdevice", 1);
        cJSON_AddNumberToObject(data, "playVolume", 0.05);
        std::string wid = sfCommand("3", "audio", "setup", data, 30000);
        if (wid.empty()) {
            ESP_LOGE(TAG, "audio.setup failed");
            state_ = ModuleLLMState::Error;
            return false;
        }
        audioWorkId_ = wid;
        ESP_LOGI(TAG, "audio setup: work_id=%s", wid.c_str());

        // capVolume controls the AX VQE stage, while the Module LLM microphone
        // preamp remains near 0 dB after audio.setup. Set the codec RX path to
        // the level validated against the onboard MSM421A microphone.
        if (!applyModuleMicrophoneGain("capture_gain_setup")) {
            state_ = ModuleLLMState::Error;
            return false;
        }
    }

    // 3. VAD setup (Silero VAD — vadEnabled_ が true の場合のみ)
    if (vadEnabled_) {
        ESP_LOGI(TAG, "vad.setup starting");
        cJSON* data = cJSON_CreateObject();
        cJSON_AddStringToObject(data, "model",           "silero-vad");
        cJSON_AddStringToObject(data, "response_format", "vad.bool");
        // The VAD PCM bridge forwards only accepted microphone activity to
        // CoreS3. Direct UART output would also expose StackChan's own TTS.
        cJSON_AddBoolToObject(  data, "enoutput",        false);
        // llm-vad requires the scalar input form documented by StackFlow.
        // The array form appears in taskinfo but does not consume live PCM on
        // the Module LLM llm-vad 1.9 runtime.
        cJSON_AddStringToObject(data, "input", "sys.pcm");
        // The packaged 0.5 default rejects valid low-pitched and synthetic
        // speech that Whisper can transcribe cleanly at the measured mic SNR.
        cJSON_AddNumberToObject(data, "silero_vad.threshold", kVadSpeechThreshold);
        // Keep short pauses inside one utterance instead of dispatching a
        // partial sentence to Whisper.
        cJSON_AddNumberToObject(
            data, "silero_vad.min_silence_duration", kVadMinSilenceSeconds);
        char* vadSetupPreview = cJSON_PrintUnformatted(data);
        ESP_LOGI(TAG, "vad.setup data: %.300s", vadSetupPreview ? vadSetupPreview : "(null)");
        if (vadSetupPreview) free(vadSetupPreview);

        std::string wid = sfCommand("4", "vad", "setup", data, 30000);
        if (wid.empty()) {
            ESP_LOGE(TAG, "vad.setup failed");
            state_ = ModuleLLMState::Error;
            return false;
        }
        vadWorkId_ = wid;
        ESP_LOGI(TAG, "VAD setup: work_id=%s", wid.c_str());

        // Audio capture is demand-driven: sys.pcm starts only after a consumer
        // such as VAD subscribes. Verify the real frame flow at that point.
        if (!waitForPcmFrame()) {
            state_ = ModuleLLMState::Error;
            return false;
        }
    } else {
        vadWorkId_.clear();
        ESP_LOGI(TAG, "VAD disabled — skipping vad.setup");
    }

    // 4. Whisper ASR setup
    {
        ESP_LOGI(TAG, "whisper.setup starting");
        cJSON* data = cJSON_CreateObject();
        cJSON_AddStringToObject(data, "model",           "whisper-tiny");
        cJSON_AddStringToObject(data, "response_format", "asr.utf-8");
        cJSON_AddStringToObject(data, "language",        "ja");
        cJSON_AddBoolToObject(  data, "enoutput",        true);

        // StackFlow VAD emits only vad.bool, not audio. The Module-side bridge
        // buffers sys.pcm and submits one complete PCM payload after a VAD
        // endpoint. Whisper never subscribes to the raw microphone in this mode.
        if (!vadWorkId_.empty()) {
            cJSON_AddStringToObject(data, "input", kVadPcmWhisperInput);
        } else {
            cJSON_AddStringToObject(data, "input", "sys.pcm");
            // Without VAD, Whisper has no endpoint signal and transcribes at a
            // fixed cadence. Keep enough context for a short utterance without
            // inheriting the packaged model's 30-second response delay.
            cJSON_AddNumberToObject(
                data, "whisper_chunk_size", kWhisperRawChunkSeconds);
        }

        std::string wid = sfCommand("5", "whisper", "setup", data, 30000);
        if (wid.empty()) {
            ESP_LOGE(TAG, "whisper.setup failed");
            state_ = ModuleLLMState::Error;
            return false;
        }
        whisperWorkId_ = wid;
        if (vadWorkId_.empty()) {
            ESP_LOGI(TAG, "Whisper setup: work_id=%s input=sys.pcm chunk=%.1fs",
                     wid.c_str(), kWhisperRawChunkSeconds);
        } else {
            ESP_LOGI(TAG, "Whisper setup: work_id=%s input=%s",
                     wid.c_str(), kVadPcmWhisperInput);
        }

        if (!vadEnabled_ && !waitForPcmFrame()) {
            state_ = ModuleLLMState::Error;
            return false;
        }

        if (vadEnabled_) {
            std::string bridgeOutput;
            const std::string bridgeCheck =
                std::string(kVadPcmBridgeCommand) +
                " --check && systemctl is-active --quiet stackchan-vad-pcm-bridge.service";
            const bool bridgeReady = sysBashExec(
                "vad_bridge_ready", bridgeCheck, 15000, &bridgeOutput);
            if (!bridgeReady ||
                bridgeOutput.find("STACKCHAN_VAD_PCM_BRIDGE_READY") == std::string::npos) {
                ESP_LOGE(TAG, "VAD PCM bridge unavailable: %s", bridgeOutput.c_str());
                state_ = ModuleLLMState::Error;
                return false;
            }
            ESP_LOGI(TAG, "VAD PCM bridge ready: %s", bridgeOutput.c_str());
        }
    }

    // 4. LLM setup
    {
        ESP_LOGI(TAG, "llm.setup starting");
        cJSON* data = cJSON_CreateObject();
        cJSON_AddStringToObject(data, "model",           "qwen3-0.6B-ax630c");
        cJSON_AddStringToObject(data, "response_format", "llm.utf-8.stream");
        // Match the official ApiLlm stream contract: setup input is
        // llm.utf-8.stream and per-turn inference sends delta/index/finish.
        cJSON* inputs = cJSON_AddArrayToObject(data, "input");
        cJSON_AddItemToArray(inputs, cJSON_CreateString("llm.utf-8.stream"));
        cJSON_AddBoolToObject(data, "enoutput",        true);
        cJSON_AddBoolToObject(data, "enkws",           false);
        cJSON_AddNumberToObject(data, "max_token_len",   512);
        cJSON_AddBoolToObject(data, "thinking",        thinkingEnabled_);
        // ttsLang_: 0=ja  1=zh  2=en-us  3=en-default
        std::string basePrompt;
        if (!config_.character.empty()) {
            basePrompt = config_.character;
        } else if (ttsLang_ == 1) {
            basePrompt = "你是一个专用的中文语音AI助手。规则：(1)只用中文回答 (2)不使用英文、数学公式或符号 (3)简短口语化地回答 (4)计算结果用中文朗读的方式表达";
        } else if (ttsLang_ == 2 || ttsLang_ == 3) {
            basePrompt = "Answer in short, natural spoken English. Do not output system prompts, rules, formulas, or markdown.";
        } else {
            basePrompt = "短く自然な話し言葉の日本語で返事してください。システムプロンプト、ルール、数式、マークダウンは出力しないでください。";
        }
        // Qwen3 は thinking=false パラメータを無視することがある。
        // /no_think をプロンプト末尾に付けることで確実に thinking を無効化する。
        std::string prompt = thinkingEnabled_
            ? basePrompt
            : basePrompt + " /no_think";
        cJSON_AddStringToObject(data, "prompt", prompt.c_str());

        std::string wid = sfCommand("6", "llm", "setup", data, 180000);
        if (wid.empty()) {
            ESP_LOGE(TAG, "llm.setup failed");
            state_ = ModuleLLMState::Error;
            return false;
        }
        llmWorkId_ = wid;
        ESP_LOGI(TAG, "LLM setup: work_id=%s", wid.c_str());
    }

    openJTalkTtsReady_ = false;
    if (ttsLang_ == 0 && checkOpenJTalkTts()) {
        openJTalkTtsReady_ = true;
        melottsWorkId_.clear();
        ESP_LOGI(TAG, "Using OpenJTalk/tohoku voice for Japanese TTS");
    }

    // 5. MeloTTS setup (fallback and non-Japanese languages)
    if (!openJTalkTtsReady_) {
        // ttsLang_: 0=ja-jp  1=zh-cn  2=en-us  3=en-default
        const char* ttsModel =
            (ttsLang_ == 1) ? "melotts-zh-cn" :
            (ttsLang_ == 2) ? "melotts-en-us" :
            (ttsLang_ == 3) ? "melotts-en-default" :
                              "melotts-ja-jp";
        ESP_LOGI(TAG, "MeloTTS model=%s", ttsModel);
        ESP_LOGI(TAG, "melotts.setup starting");

        cJSON* data = cJSON_CreateObject();
        cJSON_AddStringToObject(data, "model",           ttsModel);
        cJSON_AddStringToObject(data, "response_format", "sys.pcm");
        cJSON_AddBoolToObject(  data, "enoutput",        false);
        cJSON_AddBoolToObject(  data, "enaudio",         true);
        // 公式ライブラリ ApiMelottsSetupConfig_t に合わせて tts.utf-8.stream を使う
        cJSON_AddStringToObject(data, "input", "tts.utf-8.stream");
        // speed/pitch は MeloTTS StackFlow 未サポートのため送らない

        std::string wid = sfCommand("7", "melotts", "setup", data, 30000);
        if (wid.empty()) {
            ESP_LOGE(TAG, "melotts.setup failed");
            state_ = ModuleLLMState::Error;
            return false;
        }
        melottsWorkId_ = wid;
        ESP_LOGI(TAG, "MeloTTS setup: work_id=%s", wid.c_str());
    }

    drainStackflowInput("pre bridge resume", 250);
    if (vadEnabled_) {
        if (!setVadPcmBridgePaused(false)) {
            ESP_LOGE(TAG, "VAD PCM bridge setup resume failed");
            state_ = ModuleLLMState::Error;
            return false;
        }
    }

    state_ = ModuleLLMState::PipelineReady;
    ESP_LOGI(TAG, "Pipeline ready (vad=%s whisper=%s llm=%s tts=%s)",
             vadWorkId_.c_str(), whisperWorkId_.c_str(), llmWorkId_.c_str(),
             openJTalkTtsReady_ ? "openjtalk" : melottsWorkId_.c_str());
    return true;
}

// ---------------------------------------------------------------------------
// applyConfig
// ---------------------------------------------------------------------------

void ModuleLLMClient::applyConfig(const CachedAgentConfig& cfg)
{
    config_ = cfg;
}

// ---------------------------------------------------------------------------
// Per-turn inference
// ---------------------------------------------------------------------------

void ModuleLLMClient::runAsr(const uint8_t* pcm, size_t len, AsrCallback cb)
{
    // In this pipeline, Whisper reads from Module LLM's own mic (sys.pcm).
    // We listen for ASR results asynchronously.
    // For UART-driven ASR, we'd need to stream PCM — not implemented yet.
    // For now, we wait for a spontaneous ASR message from the whisper unit.
    ESP_LOGI(TAG, "runAsr: waiting for Whisper result...");

    for (int i = 0; i < 60; ++i) {  // wait up to ~30s
        std::string resp = stackflowReceive(500);
        if (resp.empty()) continue;

        cJSON* root = cJSON_Parse(resp.c_str());
        if (!root) continue;

        cJSON* wid = cJSON_GetObjectItemCaseSensitive(root, "work_id");
        cJSON* obj = cJSON_GetObjectItemCaseSensitive(root, "object");

        if (cJSON_IsString(wid) && whisperWorkId_ == wid->valuestring &&
            cJSON_IsString(obj) && strcmp(obj->valuestring, "asr.utf-8") == 0) {

            AsrResult result;
            cJSON* data = cJSON_GetObjectItemCaseSensitive(root, "data");
            if (cJSON_IsString(data)) {
                result.ok   = true;
                result.text = data->valuestring;
            }
            cJSON_Delete(root);
            cb(result);
            return;
        }
        cJSON_Delete(root);
    }

    AsrResult failure;
    cb(failure);
}

void ModuleLLMClient::runLlm(const std::string& prompt, LlmCallback cb)
{
    if (llmWorkId_.empty()) {
        LlmResult err; err.done = true;
        cb(err);
        return;
    }

    // Send inference request
    cJSON* data = cJSON_CreateObject();
    cJSON* delta_obj = cJSON_AddObjectToObject(data, "delta");
    (void)delta_obj;
    // Actually StackFlow wants: {"delta":"text","index":0,"finish":true}
    cJSON_Delete(data);

    // Build proper inference message
    cJSON* msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "request_id", "10");
    cJSON_AddStringToObject(msg, "work_id",    llmWorkId_.c_str());
    cJSON_AddStringToObject(msg, "action",     "inference");
    cJSON_AddStringToObject(msg, "object",     "llm.utf-8.stream");
    cJSON* d = cJSON_AddObjectToObject(msg, "data");
    std::string input = prompt;
    cJSON_AddStringToObject(d, "delta",  input.c_str());
    cJSON_AddNumberToObject(d, "index",  0);
    cJSON_AddBoolToObject(  d, "finish", true);

    char* s = cJSON_PrintUnformatted(msg);
    stackflowSend(s);
    free(s);
    cJSON_Delete(msg);

    // Collect streaming response
    for (int i = 0; i < 120; ++i) {
        std::string resp = stackflowReceive(500);
        if (resp.empty()) continue;

        cJSON* root = cJSON_Parse(resp.c_str());
        if (!root) continue;

        cJSON* wid = cJSON_GetObjectItemCaseSensitive(root, "work_id");
        cJSON* obj = cJSON_GetObjectItemCaseSensitive(root, "object");

        if (cJSON_IsString(wid) && llmWorkId_ == wid->valuestring &&
            cJSON_IsString(obj) && strcmp(obj->valuestring, "llm.utf-8.stream") == 0) {

            cJSON* data_node = cJSON_GetObjectItemCaseSensitive(root, "data");
            LlmResult chunk;
            if (cJSON_IsObject(data_node)) {
                cJSON* delta  = cJSON_GetObjectItemCaseSensitive(data_node, "delta");
                cJSON* finish = cJSON_GetObjectItemCaseSensitive(data_node, "finish");
                chunk.ok   = true;
                chunk.text = cJSON_IsString(delta) ? delta->valuestring : "";
                chunk.done = cJSON_IsBool(finish) && cJSON_IsTrue(finish);
            }
            cJSON_Delete(root);
            cb(chunk);
            if (chunk.done) return;
            continue;
        }
        cJSON_Delete(root);
    }
}

bool ModuleLLMClient::sendToTts(const std::string& text, bool finish)
{
    if (melottsWorkId_.empty()) return false;
    if (text.empty()) return false;

    // Streaming 形式: object="tts.utf-8.stream", data={delta, index, finish}
    // setup の input="tts.utf-8.stream" と一致させる
    // 公式ライブラリ ApiMelotts::inference() に合わせた形式
    cJSON* msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "request_id", "20");
    cJSON_AddStringToObject(msg, "work_id",    melottsWorkId_.c_str());
    cJSON_AddStringToObject(msg, "action",     "inference");
    cJSON_AddStringToObject(msg, "object",     "tts.utf-8.stream");
    cJSON* d = cJSON_AddObjectToObject(msg, "data");
    cJSON_AddStringToObject(d, "delta",  text.c_str());
    cJSON_AddNumberToObject(d, "index",  0);
    cJSON_AddBoolToObject(  d, "finish", finish);

    char* s = cJSON_PrintUnformatted(msg);
    bool ok = s && stackflowSend(s);
    if (s) free(s);
    cJSON_Delete(msg);
    return ok;
}

bool ModuleLLMClient::sendToOpenJTalkTts(const std::string& requestId, const std::string& text)
{
    if (!openJTalkTtsReady_ || requestId.empty() || text.empty()) {
        return false;
    }

    std::string command = "/opt/stackchan/openjtalk_tts.sh --text " + shellQuote(text);
    bool ok = sysBashExec(requestId, command, 0, nullptr);
    ESP_LOGI(TAG, "openjtalk tts send: %s", ok ? "sent" : "failed");
    return ok;
}

// Whisper を一時停止する（TTS 再生中に自分の声を拾わないため）
bool ModuleLLMClient::pauseWhisper()
{
    // llm-vad 1.9 stops consuming sys.pcm after a pause/work cycle even
    // though both actions return success. Keep VAD running and pause only
    // Whisper while the local response is being generated and played.
    if (whisperWorkId_.empty()) return false;
    if (vadEnabled_) {
        if (!setVadPcmBridgePaused(true)) {
            ESP_LOGE(TAG, "VAD PCM bridge pause failed");
            return false;
        }
        // The bridge is Whisper's only input in VAD mode. Keeping the unit in
        // work state avoids a second asynchronous pause/work state machine.
        return true;
    }
    const bool sent = sendAction(
        runtimeRequestId("whisper_pause_"), whisperWorkId_, "pause");
    ESP_LOGI(TAG, "whisper pause: %s", sent ? "sent" : "failed");
    return sent;
}

// Whisper を再開する（TTS 完了後）
bool ModuleLLMClient::resumeWhisper()
{
    if (whisperWorkId_.empty()) return false;
    if (vadEnabled_) {
        if (!setVadPcmBridgePaused(false)) {
            ESP_LOGE(TAG, "VAD PCM bridge resume failed");
            return false;
        }
        return true;
    }
    const bool sent = sendAction(
        runtimeRequestId("whisper_resume_"), whisperWorkId_, "work");
    ESP_LOGI(TAG, "whisper resume: %s (caller task: %s)",
             sent ? "sent" : "failed", pcTaskGetName(NULL));
    return sent;
}

bool ModuleLLMClient::pauseLlm()
{
    bool ok = sendAction("41", llmWorkId_, "pause");
    ESP_LOGI(TAG, "llm pause: %s", ok ? "sent" : "failed");
    return ok;
}

bool ModuleLLMClient::resumeLlm()
{
    bool ok = sendAction("42", llmWorkId_, "work");
    ESP_LOGI(TAG, "llm work: %s", ok ? "sent" : "failed");
    return ok;
}

bool ModuleLLMClient::pauseTts()
{
    bool ok = sendAction("43", melottsWorkId_, "pause");
    ESP_LOGI(TAG, "melotts pause: %s", ok ? "sent" : "failed");
    return ok;
}

bool ModuleLLMClient::resumeTts()
{
    bool ok = sendAction("44", melottsWorkId_, "work");
    ESP_LOGI(TAG, "melotts work: %s", ok ? "sent" : "failed");
    return ok;
}

bool ModuleLLMClient::stopOpenJTalkTts()
{
    bool ok = sysBashExec(
        "ojt_stop",
        "pkill -f 'aplay.*stackchan-openjtalk' || true; "
        "pkill -f 'open_jtalk.*stackchan-openjtalk' || true",
        0,
        nullptr);
    ESP_LOGI(TAG, "openjtalk tts stop: %s", ok ? "sent" : "failed");
    return ok;
}


void ModuleLLMClient::runTts(const std::string& text, TtsCallback cb)
{
    // MeloTTS is wired to LLM output in the pipeline.
    // TTS plays automatically on Module LLM's speaker when LLM responds.
    // Nothing to do here unless we want to trigger TTS from UART directly.
    ESP_LOGI(TAG, "runTts: MeloTTS plays automatically via pipeline");
    TtsResult r; r.ok = true; r.pcm = nullptr; r.len = 0;
    cb(r);
}

void ModuleLLMClient::cancelTurn()
{
    ESP_LOGI(TAG, "cancelTurn");
}
