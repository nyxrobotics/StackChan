#include "module_llm_backend.h"

#include <hal/board/hal_bridge.h>   // app_output_pcm
#include <hal/hal_bridge_conv.h>    // notify_turn_start / notify_turn_end
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cJSON.h>
#include <cstdio>
#include <cstring>
#include <sstream>

static const char* TAG = "ModLLMBackend";

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

ModuleLLMBackend::ModuleLLMBackend(std::shared_ptr<ModuleLLMClient> client)
    : client_(std::move(client))
{}

ModuleLLMBackend::~ModuleLLMBackend() {
    stop();
    taskRunning_.store(false);
    for (int i = 0; pollTask_ != nullptr && i < 10; ++i) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void ModuleLLMBackend::start() {
    if (active_.exchange(true)) {
        ESP_LOGD(TAG, "start ignored: already active");
        return;
    }

    if (pollTask_ != nullptr) {
        ESP_LOGI(TAG, "start: reusing existing pollLoop task");
        return;
    }

    // Launch UART polling task — reads Whisper ASR results and drives LLM
    taskRunning_.store(true);
    BaseType_t created = xTaskCreate([](void* arg) {
        static_cast<ModuleLLMBackend*>(arg)->pollLoop();
        vTaskDelete(nullptr);
    }, "modllm_poll", 8192, this, 5, &pollTask_);

    if (created != pdPASS) {
        active_.store(false);
        taskRunning_.store(false);
        pollTask_ = nullptr;
        ESP_LOGE(TAG, "pollLoop task creation failed");
        if (onFailure_) onFailure_(BackendKind::ModuleLLM);
        return;
    }

    ESP_LOGI(TAG, "pollLoop task created");
}

void ModuleLLMBackend::stop() {
    if (!active_.exchange(false)) {
        ESP_LOGD(TAG, "stop ignored: already inactive");
        return;
    }
    ESP_LOGI(TAG, "stop requested");
    // The poll task remains alive and idles until start() activates it again.
}

void ModuleLLMBackend::beginTurn() { ESP_LOGD(TAG, "beginTurn"); }
void ModuleLLMBackend::endTurn() {
    // LocalOnly では StateMachine がすぐ idle に遷移するため、
    // ここで resumeWhisper すると LLM/TTS 完了前に Whisper が再開してしまう。
    // resume は MeloTTS の UART 応答のみで行う。
    ESP_LOGD(TAG, "endTurn (whisper resume deferred to MeloTTS response)");
}

std::string ModuleLLMBackend::nextRequestId(const char* prefix) {
    char buf[32];
    uint32_t seq = requestSeq_.fetch_add(1) + 1;
    snprintf(buf, sizeof(buf), "%s%lu", prefix, static_cast<unsigned long>(seq));
    return std::string(buf);
}

void ModuleLLMBackend::resumePausedUnitsForNextTurn() {
    if (!client_) return;

    if (llmPausedForAbort_) {
        client_->resumeLlm();
        llmPausedForAbort_ = false;
    }
}

void ModuleLLMBackend::finishLocalTurn(const char* reason) {
    ESP_LOGI(TAG, "Local turn finished: %s", reason ? reason : "done");
    hal_bridge::notify_local_tts_end();
    pendingTts_.clear();
    inThinkBlock_ = false;
    currentLlmRequestId_.clear();
    currentTtsRequestId_.clear();
    if (client_) client_->resumeWhisper();
    micMuted_      = false;
    ttsDispatched_ = false;
}

void ModuleLLMBackend::handleAbortRequest() {
    ESP_LOGI(TAG, "Abort requested by touch");

    if (client_) {
        if (ttsDispatched_) {
            if (!currentTtsRequestId_.empty()) {
                client_->stopOpenJTalkTts();
            } else {
                client_->pauseTts();
                ttsPausedForAbort_ = true;
            }
        } else if (!currentLlmRequestId_.empty()) {
            client_->pauseLlm();
            llmPausedForAbort_ = true;
        }
    }

    finishLocalTurn("abort");
}

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

void ModuleLLMBackend::applyConfig(const CachedAgentConfig& cfg) {
    config_ = cfg;
    if (client_) client_->applyConfig(cfg);
}

// ---------------------------------------------------------------------------
// filterThinkTags
// <think>...</think> ブロックを chunk から除去する。
// ブロックが複数チャンクにまたがる場合に備えて inBlock フラグを引き継ぐ。
// chunk は in-place で書き換えられる。戻り値は新しい inBlock 状態。
// ---------------------------------------------------------------------------

static bool isEnglishTtsLang(uint8_t lang)
{
    return lang == 2 || lang == 3;
}

// sanitizeTtsText — TTS に送る前に読み上げ不可能な記法を除去する
// 処理順:
//   1. LaTeX コマンド・Markdown 記号を除去
//   2. 括弧（ASCII/全角/【】/「」等）とその内容を除去
//   3. 文末の記号を除去（! ? ！ ？ は1個まで残す）
static void sanitizeTtsText(std::string& text, uint8_t lang)
{
    const bool english = isEnglishTtsLang(lang);

    // --- Step 1: LaTeX・Markdown 除去 ---
    {
        std::string r;
        r.reserve(text.size());
        const unsigned char* p = reinterpret_cast<const unsigned char*>(text.c_str());
        while (*p) {
            if (*p == '\\') {
                ++p;
                while (*p && ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z'))) ++p;
                continue;
            }
            if (*p == '*' || *p == '#' || *p == '^' || *p == '_') { ++p; continue; }
            if (*p == '{' || *p == '}') { ++p; continue; }
            // マルチバイト
            if (*p & 0x80) {
                // 「U+300C」U+300D『U+300E』U+300F の括弧文字のみ除去（中身は残す）
                static const char* kQuoteChars[] = {
                    "\xE3\x80\x8C", "\xE3\x80\x8D",  // 「」
                    "\xE3\x80\x8E", "\xE3\x80\x8F",  // 『』
                    nullptr
                };
                bool isQuote = false;
                for (int qi = 0; kQuoteChars[qi]; qi++) {
                    if (memcmp(p, kQuoteChars[qi], 3) == 0) { p += 3; isQuote = true; break; }
                }
                if (isQuote) continue;
                int len = (*p & 0xE0) == 0xC0 ? 2 : (*p & 0xF0) == 0xE0 ? 3 : 4;
                for (int i = 0; i < len && *p; i++) r += (char)*p++;
                continue;
            }
            // ASCII: 日本語/中国語 TTS では英字を落とし、英語 TTS では英字を保持する。
            if (english &&
                (((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')) ||
                 (*p >= '0' && *p <= '9') ||
                 *p == ',' || *p == '.' || *p == '!' || *p == '?' ||
                 *p == ';' || *p == ':' || *p == '\'' || *p == '"' ||
                 *p == '-' || *p == '\n' || *p == ' ')) {
                r += (char)*p++;
                continue;
            }
            if (!english &&
                ((*p >= '0' && *p <= '9') ||
                 *p == ',' || *p == '.' || *p == '!' || *p == '?' ||
                 *p == '\n' || *p == ' ')) {
                r += (char)*p++;
                continue;
            }
            ++p;  // 英字・その他ASCII記号はスキップ
        }
        text = r;
    }

    // --- Step 2: 括弧とその内容を除去 ---
    // 対応ペア: () [] （）【】「」『』〔〕
    // ネスト非対応（1段のみ）
    {
        // 全角括弧の UTF-8 バイト列
        static const char kAsciiOpen[]  = "([";
        static const char kAsciiClose[] = ")]";
        // 全角括弧は3バイト: U+FF08（）U+3010【U+3011】U+300C「U+300D」U+300E『U+300F』U+3014〔U+3015〕
        static const struct { const char* open; const char* close; } kWide[] = {
            {"\xEF\xBC\x88", "\xEF\xBC\x89"},  // （）← 注釈括弧
            {"\xE3\x80\x90", "\xE3\x80\x91"},  // 【】← 注釈括弧
            {"\xE3\x80\x94", "\xE3\x80\x95"},  // 〔〕← 注釈括弧
            // 「」『』は引用符のため除外（中身を消さない）
        };

        std::string r;
        r.reserve(text.size());
        const unsigned char* p = reinterpret_cast<const unsigned char*>(text.c_str());
        while (*p) {
            // ASCII 括弧チェック
            bool skipAscii = false;
            for (int i = 0; kAsciiOpen[i]; i++) {
                if (*p == (unsigned char)kAsciiOpen[i]) {
                    ++p;
                    while (*p && *p != (unsigned char)kAsciiClose[i]) {
                        if (*p & 0x80) {
                            int len = (*p & 0xE0) == 0xC0 ? 2 : (*p & 0xF0) == 0xE0 ? 3 : 4;
                            p += len;
                        } else ++p;
                    }
                    if (*p) ++p;  // 閉じ括弧をスキップ
                    skipAscii = true;
                    break;
                }
            }
            if (skipAscii) continue;

            // 全角括弧チェック
            bool skipWide = false;
            if (*p & 0x80) {
                for (auto& pair : kWide) {
                    if (memcmp(p, pair.open, 3) == 0) {
                        p += 3;
                        while (*p) {
                            if ((*p & 0x80) && memcmp(p, pair.close, 3) == 0) { p += 3; break; }
                            if (*p & 0x80) {
                                int len = (*p & 0xE0) == 0xC0 ? 2 : (*p & 0xF0) == 0xE0 ? 3 : 4;
                                p += len;
                            } else ++p;
                        }
                        skipWide = true;
                        break;
                    }
                }
            }
            if (skipWide) continue;

            // 通常文字はそのまま
            if (*p & 0x80) {
                int len = (*p & 0xE0) == 0xC0 ? 2 : (*p & 0xF0) == 0xE0 ? 3 : 4;
                for (int i = 0; i < len && *p; i++) r += (char)*p++;
            } else {
                r += (char)*p++;
            }
        }
        text = r;
    }

    // --- Step 3: 文末の記号を除去（! ? ！ ？ は1個まで残す）---
    // 「記号」= ASCII の非英数字・非空白、または全角句読点類
    // ただし ！（U+FF01）と ？（U+FF1F）は例外
    static const char* kBang  = "\xEF\xBC\x81";  // ！
    static const char* kQuery = "\xEF\xBC\x9F";  // ？

    // 文末から逆方向にスキャンして「最後の本文文字」位置を探す
    // 本文文字 = 日本語かな漢字、英数字、または通常の句読点以外
    // ここでは末尾から記号のみで構成されるスパンを切り取る
    if (!text.empty()) {
        // 末尾のトレイリング記号を集める（逆順）
        bool foundBang = false, foundQuery = false;
        // 末尾から本文文字が出るまで戻る
        // バイト列を末尾から解析（UTF-8 は末尾バイトの範囲で判別可能）
        size_t end = text.size();
        while (end > 0) {
            // 3バイト文字の末尾か確認
            if (end >= 3) {
                const char* tail = text.c_str() + end - 3;
                if (memcmp(tail, kBang,  3) == 0) { foundBang  = true; end -= 3; continue; }
                if (memcmp(tail, kQuery, 3) == 0) { foundQuery = true; end -= 3; continue; }
                // 他の全角句読点（。、・…—〜）
                // U+3000-U+303F: 3バイト E3 80 80-BF
                unsigned char b0 = (unsigned char)tail[0];
                unsigned char b1 = (unsigned char)tail[1];
                if (b0 == 0xE3 && b1 == 0x80) { end -= 3; continue; }
                // U+FF00-U+FFEF: EF BC-BF
                if (b0 == 0xEF && (b1 == 0xBC || b1 == 0xBD)) { end -= 3; continue; }
            }
            // ASCII 1バイト記号
            unsigned char c = (unsigned char)text[end - 1];
            if (c < 0x80) {
                if (c == '!' ) { foundBang  = true; end--; continue; }
                if (c == '?' ) { foundQuery = true; end--; continue; }
                // その他のASCII記号（句読点・スペース等）
                if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || c == '\n')) {
                    end--; continue;
                }
            }
            break;  // 本文文字に到達
        }
        text = text.substr(0, end);
        // ! ? を1個まで付加（! 優先）
        if (foundBang)       text += "！";
        else if (foundQuery) text += "？";
    }

    // --- Step 4: 日本語（3バイトUTF-8）を含まない行を除去 ---
    // 英語 thinking が本文に漏れた場合の残骸（", . , ."等）を除去
    if (!english) {
        std::string result;
        std::istringstream ss(text);
        std::string line;
        while (std::getline(ss, line)) {
            // 3バイトUTF-8文字が含まれるか確認
            bool hasJapanese = false;
            const unsigned char* p = reinterpret_cast<const unsigned char*>(line.c_str());
            while (*p) {
                if ((*p & 0xF0) == 0xE0) { hasJapanese = true; break; }
                if (*p & 0x80) p += ((*p & 0xE0) == 0xC0) ? 2 : 4;
                else ++p;
            }
            if (hasJapanese) {
                if (!result.empty()) result += '\n';
                result += line;
            }
        }
        text = result;
    }
}


static bool filterThinkTags(std::string& chunk, bool inBlock)
{
    std::string result;
    size_t i = 0;
    while (i < chunk.size()) {
        if (!inBlock) {
            size_t open = chunk.find("<think>", i);
            if (open == std::string::npos) {
                result += chunk.substr(i);
                break;
            }
            result += chunk.substr(i, open - i);
            i = open + 7;  // skip "<think>"
            inBlock = true;
        } else {
            size_t close = chunk.find("</think>", i);
            if (close == std::string::npos) {
                // タグが閉じていない — 次のチャンクまで待つ
                break;
            }
            i = close + 8;  // skip "</think>"
            inBlock = false;
            // </think> 直後の改行・空白を読み飛ばす
            while (i < chunk.size() && (chunk[i] == '\n' || chunk[i] == '\r' || chunk[i] == ' ')) {
                ++i;
            }
        }
    }
    chunk = result;
    return inBlock;
}

// ---------------------------------------------------------------------------
// pollLoop — runs in background task
// Continuously reads UART from Module LLM.
// When Whisper sends an ASR result, forward to LLM for inference.
// ---------------------------------------------------------------------------

void ModuleLLMBackend::pollLoop() {
    ESP_LOGI(TAG, "pollLoop started");

    int64_t ttsDispatchedMs = 0;  // TTS 送信時刻
    int64_t ttsTimeoutMs = 10000; // 文字数に応じて動的に設定

    // 言語別タイムアウト係数
    // BASE: TTS 生成開始までの余裕
    // PER_CHAR: 通常文字1バイトあたり (JA/ZH は3バイト=1文字なので÷3相当)
    // PUNCT: 句読点1個あたりの追加時間
    struct TtsTimeParams { int64_t base; int64_t perByte; int64_t punct; };
    // lang: 0=ja 1=zh 2=en-us 3=en-default
    static constexpr TtsTimeParams kTtsParams[4] = {
        {1000,  40, 350},  // 0: Japanese  (40ms/byte × 3byte = 120ms/char)
        {1000,  43, 350},  // 1: Chinese   (43ms/byte × 3byte = 130ms/char)
        {1500,  70, 280},  // 2: English US
        {1500,  70, 280},  // 3: English default
    };

    // 句読点バイト列の先頭バイト判定用ヘルパー（UTF-8）
    // 対象: 。、！？…・（全角3バイト）, . , ! ? ; : ' " - （ASCII 1バイト）
    auto isPunct = [](const unsigned char* p) -> bool {
        // ASCII 句読点
        if (*p < 0x80) {
            char c = (char)*p;
            return c == '.' || c == ',' || c == '!' || c == '?' ||
                   c == ';' || c == ':' || c == '-' || c == '\'' || c == '"';
        }
        // 全角句読点 (U+3000-U+303F: E3 80 xx)
        if (p[0] == 0xE3 && p[1] == 0x80) return true;
        // 全角記号 (U+FF01-U+FF0F etc: EF BC xx / EF BD xx)
        if (p[0] == 0xEF && (p[1] == 0xBC || p[1] == 0xBD)) return true;
        return false;
    };

    while (taskRunning_.load()) {
        if (!active_.load()) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        if (!client_ || !client_->isReady()) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        bool abortRequested = abortRequested_.exchange(false);
        if (abortRequested) {
            if (micMuted_) {
                handleAbortRequest();
                ttsDispatchedMs = 0;
                continue;
            }
            ESP_LOGD(TAG, "abort ignored: local turn is not active");
        }

        // TTS送信済みかつ micMuted_ 中は タイムアウトで Whisper resume
        if (micMuted_ && ttsDispatched_) {
            int64_t now = esp_timer_get_time() / 1000;
            if (ttsDispatchedMs == 0) {
                ttsDispatchedMs = now;  // 初回: 送信時刻を記録
            } else if (now - ttsDispatchedMs >= ttsTimeoutMs) {
                if (!currentTtsRequestId_.empty() && client_) {
                    client_->stopOpenJTalkTts();
                }
                finishLocalTurn("tts timeout");
                ttsDispatchedMs = 0;
            }
        } else {
            ttsDispatchedMs = 0;
        }

        // Block up to 200ms waiting for a UART message
        std::string msg = client_->stackflowReceive(200);
        if (msg.empty()) continue;
        ESP_LOGI(TAG, "UART rx: %.500s", msg.c_str());

        // Parse JSON
        cJSON* root = cJSON_Parse(msg.c_str());
        if (!root) continue;

        cJSON* rid = cJSON_GetObjectItemCaseSensitive(root, "request_id");
        cJSON* wid = cJSON_GetObjectItemCaseSensitive(root, "work_id");
        cJSON* obj = cJSON_GetObjectItemCaseSensitive(root, "object");
        cJSON* err = cJSON_GetObjectItemCaseSensitive(root, "error");

        // Skip error responses
        if (cJSON_IsObject(err)) {
            cJSON* code = cJSON_GetObjectItemCaseSensitive(err, "code");
            if (cJSON_IsNumber(code) && code->valueint != 0) {
                cJSON_Delete(root);
                continue;
            }
        }

        if (cJSON_IsString(rid) && !currentTtsRequestId_.empty() &&
            currentTtsRequestId_ == rid->valuestring) {
            cJSON* data = cJSON_GetObjectItemCaseSensitive(root, "data");
            if (cJSON_IsString(data)) {
                ESP_LOGI(TAG, "OpenJTalk TTS result: %.200s", data->valuestring);
            }
            finishLocalTurn("openjtalk done");
            ttsDispatchedMs = 0;
            cJSON_Delete(root);
            continue;
        }

        // VAD 応答: 音声検出状態をログ出力
        if (cJSON_IsString(wid) && strncmp(wid->valuestring, "vad.", 4) == 0) {
            cJSON* data = cJSON_GetObjectItemCaseSensitive(root, "data");
            // VAD は vad.bool を返す: data が true=speech / false=silence
            if (cJSON_IsBool(data)) {
                bool speech = cJSON_IsTrue(data);
                int speechInt = speech ? 1 : 0;
                if (speechInt != lastVadSpeech_) {
                    lastVadSpeech_ = speechInt;
                    ESP_LOGI(TAG, "VAD: %s", speech ? "SPEECH" : "silence");
                }
            } else if (cJSON_IsString(data)) {
                ESP_LOGD(TAG, "VAD ack: %s", data->valuestring);
            }
            cJSON_Delete(root);
            continue;
        }

        // MeloTTS 応答: 無視する（完了検知はタイムアウトで行う）
        if (cJSON_IsString(wid) && strncmp(wid->valuestring, "melotts.", 8) == 0) {
            cJSON_Delete(root);
            continue;
        }

        // Whisper ASR result: work_id=whisper.xxxx, object=asr.utf-8
        if (cJSON_IsString(wid) && cJSON_IsString(obj) &&
            strncmp(wid->valuestring, "whisper.", 8) == 0 &&
            strcmp(obj->valuestring, "asr.utf-8") == 0)
        {
            cJSON* data = cJSON_GetObjectItemCaseSensitive(root, "data");
            std::string text = cJSON_IsString(data) ? data->valuestring : "";

            if (!text.empty()) {
                ESP_LOGI(TAG, "ASR: %s", text.c_str());
                cJSON_Delete(root);
                onAsrResult(text);
                continue;
            }
        }

        // LLM stream: work_id=llm.xxxx, object=llm.utf-8.stream
        // <think>...</think> をフィルタして MeloTTS に転送
        if (cJSON_IsString(wid) && cJSON_IsString(obj) &&
            strncmp(wid->valuestring, "llm.", 4) == 0 &&
            strcmp(obj->valuestring, "llm.utf-8.stream") == 0)
        {
            if (!cJSON_IsString(rid) || currentLlmRequestId_.empty() ||
                currentLlmRequestId_ != rid->valuestring) {
                ESP_LOGD(TAG, "LLM stream ignored: request_id=%s current=%s",
                         cJSON_IsString(rid) ? rid->valuestring : "(none)",
                         currentLlmRequestId_.c_str());
                cJSON_Delete(root);
                continue;
            }

            cJSON* data_node = cJSON_GetObjectItemCaseSensitive(root, "data");
            if (cJSON_IsObject(data_node)) {
                cJSON* delta  = cJSON_GetObjectItemCaseSensitive(data_node, "delta");
                cJSON* finish = cJSON_GetObjectItemCaseSensitive(data_node, "finish");
                std::string chunk = cJSON_IsString(delta) ? delta->valuestring : "";
                bool done = cJSON_IsBool(finish) && cJSON_IsTrue(finish);

                // <think>...</think> フィルタ
                inThinkBlock_ = filterThinkTags(chunk, inThinkBlock_);

                // 先頭の改行・空白を除去（</think> 直後のゴミ対策）
                size_t start = chunk.find_first_not_of("\n\r ");
                if (start != std::string::npos) chunk = chunk.substr(start);
                else chunk.clear();

                if (!chunk.empty()) {
                    pendingTts_ += chunk;
                }
                if (done) {
                    currentLlmRequestId_.clear();
                    if (!pendingTts_.empty()) {
                        uint8_t lang = client_->getTtsLang();
                        if (lang >= 4) lang = 0;

                        // TTS送信前に読み上げ不可能な記法を除去
                        sanitizeTtsText(pendingTts_, lang);
                        // JA/ZH では英語 thinking の残骸を弾くため CJK 比率を確認する。
                        // EN では ASCII 英字を正規の発話として許可する。
                        int nonSpaceBytes = 0;
                        int cjkBytes = 0;
                        {
                            const unsigned char* cp2 = reinterpret_cast<const unsigned char*>(pendingTts_.c_str());
                            while (*cp2) {
                                if ((*cp2 & 0xF0) == 0xE0) {
                                    cjkBytes += 3; nonSpaceBytes += 3; cp2 += 3;
                                } else if (*cp2 & 0x80) {
                                    nonSpaceBytes += 2; cp2 += 2;
                                } else {
                                    if (*cp2 != ' ' && *cp2 != '\n' && *cp2 != '\r' && *cp2 != '\t')
                                        nonSpaceBytes++;
                                    cp2++;
                                }
                            }
                        }
                        bool languageOk = nonSpaceBytes > 0;
                        if (languageOk && !isEnglishTtsLang(lang)) {
                            languageOk = cjkBytes * 10 >= nonSpaceBytes * 8;  // 80%以上
                        }
                        if (pendingTts_.empty() || !languageOk) {
                            ESP_LOGW(TAG, "LLM→TTS: skipped (lang=%u cjk=%d%% of %d non-space bytes)",
                                     lang,
                                     nonSpaceBytes > 0 ? cjkBytes * 100 / nonSpaceBytes : 0,
                                     nonSpaceBytes);
                            finishLocalTurn("tts skipped");
                            goto skip_tts_dispatch;
                        }
                        // 言語別・句読点考慮タイムアウト計算
                        {
                            const auto& tp = kTtsParams[lang];

                            int64_t totalBytes = 0;
                            int64_t punctCount = 0;
                            const unsigned char* p = reinterpret_cast<const unsigned char*>(pendingTts_.c_str());
                            while (*p) {
                                if (isPunct(p)) punctCount++;
                                int blen = (*p & 0x80) == 0 ? 1 :
                                           (*p & 0xE0) == 0xC0 ? 2 :
                                           (*p & 0xF0) == 0xE0 ? 3 : 4;
                                // 漢字（CJK U+4E00-U+9FFF: E4 B8..E9 BF）は
                                // ひらがな・カタカナの2倍の時間を見積もる（日本語のみ）
                                if (blen == 3 && lang == 0) {
                                    uint8_t b0 = p[0], b1 = p[1];
                                    bool isKanji = (b0 >= 0xE4 && b0 <= 0xE8) ||
                                                   (b0 == 0xE9 && b1 <= 0xBF);
                                    totalBytes += isKanji ? blen * 2 : blen;
                                } else {
                                    totalBytes += blen;
                                }
                                p += blen;
                            }
                            ttsTimeoutMs = tp.base
                                         + tp.perByte * totalBytes
                                         + tp.punct   * punctCount;
                            ESP_LOGI(TAG, "LLM→TTS: %s (bytes=%d punct=%d timeout=%dms)",
                                     pendingTts_.c_str(),
                                     (int)totalBytes, (int)punctCount,
                                     (int)ttsTimeoutMs);
                        }
                        if (ttsPausedForAbort_) {
                            client_->resumeTts();
                            ttsPausedForAbort_ = false;
                        }
                        if (client_->isOpenJTalkTtsReady() && lang == 0) {
                            currentTtsRequestId_ = nextRequestId("ojt_");
                            ttsTimeoutMs += 5000;  // open_jtalk + aplay startup margin
                            ttsDispatched_ = true;
                            if (!client_->sendToOpenJTalkTts(currentTtsRequestId_, pendingTts_)) {
                                ESP_LOGE(TAG, "LLM→OpenJTalk send failed");
                                finishLocalTurn("openjtalk send failed");
                                goto skip_tts_dispatch;
                            }
                        } else {
                            currentTtsRequestId_.clear();
                            ttsDispatched_ = true;
                            if (!client_->sendToTts(pendingTts_, true)) {
                                ESP_LOGE(TAG, "LLM→TTS send failed");
                                finishLocalTurn("tts send failed");
                                goto skip_tts_dispatch;
                            }
                        }
                        pendingTts_.clear();
                    } else {
                        finishLocalTurn("empty llm response");
                    }
                }
            }
            skip_tts_dispatch:
            cJSON_Delete(root);
            continue;
        }

        cJSON_Delete(root);
    }

    active_.store(false);
    pollTask_ = nullptr;
    ESP_LOGI(TAG, "pollLoop exit");
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// isAsrNoise — ノイズと判定する ASR 結果のフィルタ
// 新しいパターンはここに追加する
// ---------------------------------------------------------------------------

static std::string stripTrailing(const std::string& s) {
    std::string t = s;
    while (!t.empty() && (t.back() == '.' || t.back() == ' ')) t.pop_back();
    return t;
}

static bool isAsrNoise(const std::string& text) {
    std::string t = stripTrailing(text);
    if (t.empty()) return true;

    // ---- パターン1: カッコだけ (笑) （笑） 等 ----
    if (t.front() == '(' && t.back() == ')') return true;

    static const std::string kOpen  = "\xEF\xBC\x88";
    static const std::string kClose = "\xEF\xBC\x89";
    if (t.size() >= 6 &&
        t.substr(0, 3) == kOpen &&
        t.substr(t.size() - 3) == kClose) return true;

    // ---- パターン2: 同じフレーズの繰り返し（Whisper ハルシネーション）----
    // 「きてきてきて...」「2、3、2、3、...」のように途中から繰り返すケースも検出する
    // バイト列上で任意長のフレーズが5回以上連続する箇所があればノイズ
    if (t.size() >= 20) {
        // plen: 2〜20バイトを1バイト刻みで試す（ASCII+全角混在パターンも検出できる）
        for (int plen = 2; plen <= 20; plen++) {
            // 各開始位置を1バイト刻みで試す
            for (size_t start = 0; start + plen * 5 <= t.size(); start++) {
                const char* base = t.c_str() + start;
                int repeat = 1;
                size_t pos = start + plen;
                while (pos + plen <= t.size() &&
                       memcmp(base, t.c_str() + pos, plen) == 0) {
                    repeat++;
                    pos += plen;
                }
                if (repeat >= 5) return true;  // 5回以上連続 → ノイズ
            }
        }
    }

    return false;
}

// onAsrResult — called when Whisper produces text
// ---------------------------------------------------------------------------

void ModuleLLMBackend::onAsrResult(const std::string& text) {
    ESP_LOGI(TAG, "onAsrResult: %s", text.c_str());
    if (!active_.load()) {
        ESP_LOGI(TAG, "onAsrResult: backend inactive, skipping");
        return;
    }
    if (micMuted_) {
        ESP_LOGD(TAG, "onAsrResult: mic muted (LLM/TTS in progress), ignoring: %s", text.c_str());
        return;
    }

    if (isAsrNoise(text)) {
        ESP_LOGI(TAG, "onAsrResult: ignoring noise: %s", text.c_str());
        return;
    }

    // notify_turn_start/end は LocalOnly では呼ばない
    // StateMachine の即 idle 遷移によりバックエンドの再起動や
    // Whisper resume のタイミングずれが起きるため
    runLlmTts(text);
}

// ---------------------------------------------------------------------------
// processText — text → Qwen3 → MeloTTS (triggered externally)
// ---------------------------------------------------------------------------

void ModuleLLMBackend::processText(const std::string& text) {
    ESP_LOGI(TAG, "processText: %s", text.c_str());

    if (!client_ || !client_->isReady()) {
        ESP_LOGW(TAG, "client not ready");
        if (onFailure_) onFailure_(BackendKind::ModuleLLM);
        return;
    }

    runLlmTts(text);
}

// ---------------------------------------------------------------------------
// processAudio — not used in LocalOnly mode (Module LLM uses its own mic)
// ---------------------------------------------------------------------------

void ModuleLLMBackend::processAudio(const uint8_t* /*pcm*/, size_t /*len*/) {
    // Module LLM's Whisper reads from sys.pcm (its own mic).
    // ASR results arrive asynchronously via pollLoop().
    // Nothing to do here.
}

// ---------------------------------------------------------------------------
// runLlmTts — LLM に inference リクエストを送るだけ。
// レスポンスは pollLoop が受信して <think> フィルタ後に MeloTTS へ転送する。
// ---------------------------------------------------------------------------

void ModuleLLMBackend::runLlmTts(const std::string& userText) {
    if (!client_) { ESP_LOGE(TAG, "runLlmTts: client_ null"); return; }

    const std::string& llmWorkId = client_->llmWorkId();
    if (llmWorkId.empty()) {
        ESP_LOGW(TAG, "LLM work_id empty");
        if (onFailure_) onFailure_(BackendKind::ModuleLLM);
        return;
    }

    resumePausedUnitsForNextTurn();
    abortRequested_.store(false);

    inThinkBlock_ = false;
    pendingTts_.clear();
    currentTtsRequestId_.clear();
    micMuted_         = true;
    ttsDispatched_    = false;
    client_->pauseWhisper();   // LLM推論〜TTS完了まで Whisper を停止
    ESP_LOGI(TAG, "Local response start: begin mouth animation before LLM inference");
    hal_bridge::notify_local_tts_start();  // LLM推論開始時点で口パクアニメ開始
    currentLlmRequestId_ = nextRequestId("llm_");
    ESP_LOGI(TAG, "LLM inference: %s (llmWorkId=%s request_id=%s)",
             userText.c_str(), client_->llmWorkId().c_str(), currentLlmRequestId_.c_str());

    // LLM へ inference リクエスト送信（レスポンスは pollLoop で処理）
    cJSON* msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "request_id", currentLlmRequestId_.c_str());
    cJSON_AddStringToObject(msg, "work_id",    llmWorkId.c_str());
    cJSON_AddStringToObject(msg, "action",     "inference");
    cJSON_AddStringToObject(msg, "object",     "llm.utf-8");
    // Non-streaming input: data is a plain string
    cJSON_AddStringToObject(msg, "data", userText.c_str());

    char* s = cJSON_PrintUnformatted(msg);
    bool sent = s && client_->stackflowSend(s);
    if (s) free(s);
    cJSON_Delete(msg);

    if (!sent) {
        ESP_LOGE(TAG, "LLM inference send failed");
        finishLocalTurn("llm send failed");
        if (onFailure_) onFailure_(BackendKind::ModuleLLM);
    }
}

// ---------------------------------------------------------------------------
// playback — not used (MeloTTS plays on Module LLM's own speaker)
// ---------------------------------------------------------------------------

void ModuleLLMBackend::playback(const uint8_t* /*pcm*/, size_t /*len*/) {
    // No-op: MeloTTS handles playback internally on Module LLM
}
