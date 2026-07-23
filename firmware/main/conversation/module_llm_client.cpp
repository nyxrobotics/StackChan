#include "module_llm_client.h"

#include <driver/uart.h>
#include <esp_log.h>
#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs_flash.h>
#include <nvs.h>

#include <cstring>
#include <string>

// ---------------------------------------------------------------------------
// StackFlow JSON-RPC protocol
//   Send:    {"request_id":"N","work_id":"unit","action":"action"[,"object":"...","data":{...}]}\n
//   Receive: {"created":...,"error":{"code":0,"message":""},"work_id":"unit.1001",...}\n
//   code==0  → success; work_id in response is the instance ID to reuse
// ---------------------------------------------------------------------------

static const char* TAG = "ModLLMClient";

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
    if (uartFd_ >= 0) {
        uart_driver_delete(static_cast<uart_port_t>(kUartNum));
        uartFd_ = -1;
    }
}

// ---------------------------------------------------------------------------
// StackFlow helpers
// ---------------------------------------------------------------------------

bool ModuleLLMClient::stackflowSend(const std::string& jsonMsg)
{
    std::string line = jsonMsg + "\n";
    int written = uart_write_bytes(static_cast<uart_port_t>(kUartNum),
                                   line.c_str(), line.size());
    return written == static_cast<int>(line.size());
}

std::string ModuleLLMClient::stackflowReceive(int timeoutMs)
{
    std::string result;
    TickType_t  deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeoutMs);
    int depth = 0;
    bool started = false;
    bool inString = false;
    bool escape = false;

    while (true) {
        if (!started && xTaskGetTickCount() >= deadline) break;

        uint8_t ch;
        int n = uart_read_bytes(static_cast<uart_port_t>(kUartNum),
                                &ch, 1, pdMS_TO_TICKS(10));
        if (n <= 0) continue;

        result += static_cast<char>(ch);

        // 文字列リテラル内のエスケープ処理
        if (escape) { escape = false; continue; }
        if (ch == '\\' && inString) { escape = true; continue; }
        if (ch == '"') { inString = !inString; }
        if (inString) continue;

        if (ch == '{') { depth++; started = true; }
        else if (ch == '}') {
            depth--;
            if (started && depth == 0) break;  // JSON オブジェクト完了
        }
    }
    return result;
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
    bool sent = stackflowSend(s);
    free(s);
    // dataObj ownership transferred to msg
    cJSON_Delete(msg);

    if (!sent) {
        ESP_LOGE(TAG, "sfCommand(%s.%s): send failed", workId.c_str(), action.c_str());
        return "";
    }

    // Drain any buffered async messages until we find ours
    for (int attempt = 0; attempt < 10; ++attempt) {
        std::string resp = stackflowReceive(timeoutMs);
        if (resp.empty()) {
            ESP_LOGW(TAG, "sfCommand(%s.%s): timeout", workId.c_str(), action.c_str());
            return "";
        }

        cJSON* root = cJSON_Parse(resp.c_str());
        if (!root) continue;

        // Check request_id matches
        cJSON* rid = cJSON_GetObjectItemCaseSensitive(root, "request_id");
        if (!cJSON_IsString(rid) || reqId != rid->valuestring) {
            // Async message from another unit — ignore and keep waiting
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

    return "";
}

// ---------------------------------------------------------------------------
// waitForAck — used after ping (no data payload)
// ---------------------------------------------------------------------------

bool ModuleLLMClient::waitForAck(const std::string& method, int timeoutMs)
{
    std::string resp = stackflowReceive(timeoutMs);
    if (resp.empty()) {
        ESP_LOGW(TAG, "waitForAck(%s): timeout", method.c_str());
        return false;
    }

    cJSON* root = cJSON_Parse(resp.c_str());
    if (!root) {
        ESP_LOGW(TAG, "waitForAck(%s): parse error: %s", method.c_str(), resp.c_str());
        return false;
    }

    // StackFlow response: {"error":{"code":0,"message":""},...}
    bool ok = false;
    cJSON* errObj = cJSON_GetObjectItemCaseSensitive(root, "error");
    if (cJSON_IsObject(errObj)) {
        cJSON* code = cJSON_GetObjectItemCaseSensitive(errObj, "code");
        ok = cJSON_IsNumber(code) && (code->valueint == 0);
    }

    if (!ok) {
        ESP_LOGW(TAG, "waitForAck(%s): not ok — %s", method.c_str(), resp.c_str());
    }

    cJSON_Delete(root);
    return ok;
}

// ---------------------------------------------------------------------------
// killStaleTasks — sys.tasklist で残留タスクを取得して exit する
// sys.reset より大幅に高速（リセット待ち不要）
// ---------------------------------------------------------------------------

void ModuleLLMClient::killStaleTasks()
{
    // sys.tasklist が使えないため、既知の work_id パターンに exit を送る
    // StackFlow は work_id が存在しなければ無視するので安全
    static const char* kUnits[] = {"whisper", "vad", "llm", "melotts", "audio", nullptr};
    // インスタンス番号は通常 1001 から始まり、リセットするたびに +1 ずつ増える
    // 1001 〜 1030 の範囲を試せば十分（複数セッション分をカバー）
    static const int kIdMin = 1000;
    static const int kIdMax = 1030;

    int reqId = 100;
    for (int i = 0; kUnits[i]; ++i) {
        for (int id = kIdMin; id <= kIdMax; ++id) {
            char wid[32];
            snprintf(wid, sizeof(wid), "%s.%d", kUnits[i], id);

            char ridStr[16];
            snprintf(ridStr, sizeof(ridStr), "%d", reqId++);

            cJSON* ex = cJSON_CreateObject();
            cJSON_AddStringToObject(ex, "request_id", ridStr);
            cJSON_AddStringToObject(ex, "work_id",    wid);
            cJSON_AddStringToObject(ex, "action",     "exit");
            char exitObj[48];
            snprintf(exitObj, sizeof(exitObj), "%s.exit", kUnits[i]);
            cJSON_AddStringToObject(ex, "object", exitObj);
            char* s = cJSON_PrintUnformatted(ex);
            stackflowSend(s);
            free(s);
            cJSON_Delete(ex);
        }
        // 各ユニットの exit が処理されるのを少し待つ
        vTaskDelay(pdMS_TO_TICKS(200));
        // バッファを読み捨てる
        while (!stackflowReceive(100).empty()) {}
    }

    ESP_LOGI(TAG, "killStaleTasks: done");
    vTaskDelay(pdMS_TO_TICKS(300));
}

// ---------------------------------------------------------------------------
// connect() — UART open + StackFlow ping
// ---------------------------------------------------------------------------

bool ModuleLLMClient::connect()
{
    if (uartFd_ >= 0) return true;  // already open

    if (!uartInit(kUartNum, kTxPin, kRxPin, kBaud)) {
        ESP_LOGE(TAG, "UART init failed");
        return false;
    }
    uartFd_ = kUartNum;

    // Send ping — StackFlow format
    cJSON* ping = cJSON_CreateObject();
    cJSON_AddStringToObject(ping, "request_id", "1");
    cJSON_AddStringToObject(ping, "work_id",    "sys");
    cJSON_AddStringToObject(ping, "action",     "ping");
    char* pingStr = cJSON_PrintUnformatted(ping);
    bool sent = stackflowSend(pingStr);
    free(pingStr);
    cJSON_Delete(ping);

    if (!sent) {
        ESP_LOGE(TAG, "ping send failed");
        return false;
    }

    bool ack = waitForAck("sys.ping", 3000);
    if (!ack) {
        ESP_LOGW(TAG, "Module LLM did not respond to ping");
        return false;
    }

    state_ = ModuleLLMState::Connected;
    ESP_LOGI(TAG, "Module LLM connected");
    return true;
}

// ---------------------------------------------------------------------------
// loadModelsAndPipeline()
// Setup order (matches Arduino voice assistant example):
//   1. sys.reset
//   2. audio.setup
//   3. vad.setup    (Silero VAD, input: sys.pcm)
//   4. whisper.setup  (ASR from Module LLM mic, input: sys.pcm + vad_work_id)
//   5. llm.setup      (Qwen3, UART input)
//   6. melotts.setup  (MeloTTS ja-JP, input from LLM)
// ---------------------------------------------------------------------------

bool ModuleLLMClient::loadModelsAndPipeline()
{
    state_ = ModuleLLMState::ModelLoading;
    ESP_LOGI(TAG, "Setting up StackFlow units...");

    // sys.reset でサービスをクリーンな状態に戻す（公式 Arduino ライブラリと同じ）
    {
        cJSON* msg = cJSON_CreateObject();
        cJSON_AddStringToObject(msg, "request_id", "1");
        cJSON_AddStringToObject(msg, "work_id",    "sys");
        cJSON_AddStringToObject(msg, "action",     "reset");
        cJSON_AddStringToObject(msg, "object",     "sys.reset");
        cJSON_AddNullToObject(  msg, "data");
        char* s = cJSON_PrintUnformatted(msg);
        stackflowSend(s);
        free(s);
        cJSON_Delete(msg);
        // reset 完了を待つ（最大 10 秒）
        int64_t t0 = esp_timer_get_time() / 1000;
        while (esp_timer_get_time() / 1000 - t0 < 10000) {
            std::string rx = stackflowReceive(500);
            if (rx.find("\"sys.reset\"") != std::string::npos) {
                ESP_LOGI(TAG, "sys.reset done");
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    // 残留タスクを念のため exit する
    killStaleTasks();

    // 2. Audio setup
    {
        cJSON* data = cJSON_CreateObject();
        cJSON_AddNumberToObject(data, "capcard",   0);
        cJSON_AddNumberToObject(data, "capdevice", 0);
        cJSON_AddNumberToObject(data, "playcard",  0);
        cJSON_AddNumberToObject(data, "playdevice", 1);
        std::string wid = sfCommand("3", "audio", "setup", data, 30000);
        if (wid.empty()) {
            ESP_LOGE(TAG, "audio.setup failed");
            state_ = ModuleLLMState::Error;
            return false;
        }
        audioWorkId_ = wid;
        ESP_LOGI(TAG, "audio setup: work_id=%s", wid.c_str());
    }

    // 3. VAD setup (Silero VAD — vadEnabled_ が true の場合のみ)
    if (vadEnabled_) {
        cJSON* data = cJSON_CreateObject();
        cJSON_AddStringToObject(data, "model",           "silero-vad");
        cJSON_AddStringToObject(data, "response_format", "vad.bool");
        cJSON_AddBoolToObject(  data, "enoutput",        true);
        cJSON* inputs = cJSON_AddArrayToObject(data, "input");
        cJSON_AddItemToArray(inputs, cJSON_CreateString("sys.pcm"));

        std::string wid = sfCommand("4", "vad", "setup", data, 30000);
        if (wid.empty()) {
            ESP_LOGE(TAG, "vad.setup failed");
            state_ = ModuleLLMState::Error;
            return false;
        }
        vadWorkId_ = wid;
        ESP_LOGI(TAG, "VAD setup: work_id=%s", wid.c_str());
    } else {
        vadWorkId_.clear();
        ESP_LOGI(TAG, "VAD disabled — skipping vad.setup");
    }

    // 4. Whisper ASR setup (VAD あり: sys.pcm + vad_work_id、なし: sys.pcm のみ)
    {
        cJSON* data = cJSON_CreateObject();
        cJSON_AddStringToObject(data, "model",           "whisper-tiny");
        cJSON_AddStringToObject(data, "response_format", "asr.utf-8");
        cJSON_AddStringToObject(data, "language",        "ja");
        cJSON_AddBoolToObject(  data, "enoutput",        true);
        cJSON* inputs = cJSON_AddArrayToObject(data, "input");
        cJSON_AddItemToArray(inputs, cJSON_CreateString("sys.pcm"));
        if (!vadWorkId_.empty()) {
            cJSON_AddItemToArray(inputs, cJSON_CreateString(vadWorkId_.c_str()));
        }

        std::string wid = sfCommand("5", "whisper", "setup", data, 30000);
        if (wid.empty()) {
            ESP_LOGE(TAG, "whisper.setup failed");
            state_ = ModuleLLMState::Error;
            return false;
        }
        whisperWorkId_ = wid;
        ESP_LOGI(TAG, "Whisper setup: work_id=%s", wid.c_str());
    }

    // 4. LLM setup
    {
        cJSON* data = cJSON_CreateObject();
        cJSON_AddStringToObject(data, "model",           "qwen3-0.6B-ax630c");
        cJSON_AddStringToObject(data, "response_format", "llm.utf-8.stream");
        cJSON_AddStringToObject(data, "input",           "llm.utf-8");
        cJSON_AddBoolToObject(data, "enoutput",        true);
        cJSON_AddNumberToObject(data, "max_token_len",   512);
        cJSON_AddBoolToObject(data, "thinking",        thinkingEnabled_);
        // ttsLang_: 0=ja  1=zh  2=en-us  3=en-default
        std::string basePrompt;
        if (!config_.character.empty()) {
            basePrompt = config_.character;
        } else if (ttsLang_ == 1) {
            basePrompt = "你是一个专用的中文语音AI助手。规则：(1)只用中文回答 (2)不使用英文、数学公式或符号 (3)简短口语化地回答 (4)计算结果用中文朗读的方式表达";
        } else if (ttsLang_ == 2 || ttsLang_ == 3) {
            basePrompt = "You are a dedicated English voice AI assistant. Rules: (1) Answer only in English (2) No formulas or symbols (3) Keep answers short and conversational (4) Spell out numbers";
        } else {
            basePrompt = "あなたは日本語専用の音声AIアシスタントです。ルール：(1)必ず日本語のみで答える・英語で考えたり英語を出力することは絶対禁止 (2)数式・記号・英字を使わない (3)短く話し言葉で答える (4)計算結果は日本語で読み上げる形で答える";
        }
        // Qwen3 は thinking=false パラメータを無視することがある。
        // /no_think をプロンプト末尾に付けることで確実に thinking を無効化する。
        std::string prompt = thinkingEnabled_
            ? basePrompt
            : basePrompt + " /no_think";
        cJSON_AddStringToObject(data, "prompt", prompt.c_str());

        std::string wid = sfCommand("6", "llm", "setup", data, 60000);
        if (wid.empty()) {
            ESP_LOGE(TAG, "llm.setup failed");
            state_ = ModuleLLMState::Error;
            return false;
        }
        llmWorkId_ = wid;
        ESP_LOGI(TAG, "LLM setup: work_id=%s", wid.c_str());
    }

    // 5. MeloTTS setup
    {
        // ttsLang_: 0=ja-jp  1=zh-cn  2=en-us  3=en-default
        const char* ttsModel =
            (ttsLang_ == 1) ? "melotts-zh-cn" :
            (ttsLang_ == 2) ? "melotts-en-us" :
            (ttsLang_ == 3) ? "melotts-en-default" :
                              "melotts-ja-jp";
        ESP_LOGI(TAG, "MeloTTS model=%s", ttsModel);

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

    state_ = ModuleLLMState::PipelineReady;
    ESP_LOGI(TAG, "Pipeline ready (vad=%s whisper=%s llm=%s melotts=%s)",
             vadWorkId_.c_str(), whisperWorkId_.c_str(), llmWorkId_.c_str(), melottsWorkId_.c_str());
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

void ModuleLLMClient::sendToTts(const std::string& text, bool finish)
{
    if (melottsWorkId_.empty()) return;
    if (text.empty()) return;

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
    cJSON_AddBoolToObject(  d, "finish", true);

    char* s = cJSON_PrintUnformatted(msg);
    stackflowSend(s);
    free(s);
    cJSON_Delete(msg);
}

// Whisper を一時停止する（TTS 再生中に自分の声を拾わないため）
void ModuleLLMClient::pauseWhisper()
{
    // VAD も同時に停止（推論・TTS 中は不要なのでリソース節約）
    if (!vadWorkId_.empty()) {
        cJSON* msg = cJSON_CreateObject();
        cJSON_AddStringToObject(msg, "request_id", "29");
        cJSON_AddStringToObject(msg, "work_id",    vadWorkId_.c_str());
        cJSON_AddStringToObject(msg, "action",     "pause");
        char* s = cJSON_PrintUnformatted(msg);
        stackflowSend(s);
        free(s);
        cJSON_Delete(msg);
    }
    if (whisperWorkId_.empty()) return;
    cJSON* msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "request_id", "30");
    cJSON_AddStringToObject(msg, "work_id",    whisperWorkId_.c_str());
    cJSON_AddStringToObject(msg, "action",     "pause");
    char* s = cJSON_PrintUnformatted(msg);
    ESP_LOGI("ModLLMClient", "whisper+vad pause");
    stackflowSend(s);
    free(s);
    cJSON_Delete(msg);
}

// Whisper を再開する（TTS 完了後）
void ModuleLLMClient::resumeWhisper()
{
    // VAD を先に再開してから Whisper を再開する
    if (!vadWorkId_.empty()) {
        cJSON* msg = cJSON_CreateObject();
        cJSON_AddStringToObject(msg, "request_id", "32");
        cJSON_AddStringToObject(msg, "work_id",    vadWorkId_.c_str());
        cJSON_AddStringToObject(msg, "action",     "work");
        char* s = cJSON_PrintUnformatted(msg);
        stackflowSend(s);
        free(s);
        cJSON_Delete(msg);
    }
    if (whisperWorkId_.empty()) return;
    cJSON* msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "request_id", "31");
    cJSON_AddStringToObject(msg, "work_id",    whisperWorkId_.c_str());
    cJSON_AddStringToObject(msg, "action",     "work");
    char* s = cJSON_PrintUnformatted(msg);
    ESP_LOGI("ModLLMClient", "whisper+vad resume (caller task: %s)", pcTaskGetName(NULL));
    stackflowSend(s);
    free(s);
    cJSON_Delete(msg);
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
