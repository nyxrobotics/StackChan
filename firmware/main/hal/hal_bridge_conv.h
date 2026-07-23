#pragma once

// firmware/main/hal/hal_bridge_conv.h

#include <functional>
#include <cstdint>
#include <cstddef>

namespace hal_bridge {

// ---------------------------------------------------------------------------
// ConversationCallbacks
// ---------------------------------------------------------------------------
struct ConversationCallbacks {
    std::function<void()>                         onXiaozhiConnected;
    std::function<void()>                         onXiaozhiDisconnected;
    std::function<void()>                         onXiaozhiError;
    std::function<void()>                         onTurnStart;
    std::function<void()>                         onTurnEnd;
    std::function<void(const uint8_t*, size_t)>   onAudioChunk;
    // Local mode: TTS 再生開始/終了通知（口パクアニメ用）
    std::function<void()>                         onLocalTtsStart;
    std::function<void()>                         onLocalTtsEnd;
    // Local mode: 顔タッチによる中断通知
    std::function<void()>                         onLocalAbort;
};

void set_conversation_callbacks(ConversationCallbacks cbs);

// Active mode stored as int to avoid pulling in conversation_backend.h.
// Values mirror ConversationMode: Auto=0, LocalOnly=1, OnlineOnly=2.
static constexpr int kModeAuto       = 0;
static constexpr int kModeOnlineOnly = 1;
static constexpr int kModeLocalOnly  = 2;

void set_conversation_mode(int mode);
int  get_conversation_mode();

void notify_xiaozhi_connected();
void notify_xiaozhi_disconnected();
void notify_xiaozhi_error();
void notify_turn_start();
void notify_turn_end();
void notify_audio_chunk(const uint8_t* pcm, size_t len);
// Local mode notifications
void notify_local_tts_start();
void notify_local_tts_end();
bool is_local_tts_active();  // タッチ中断判定用
void set_conv_ready(bool ready);  // conv 初期化完了通知
bool is_conv_ready();             // popup 音のタイミング制御用
void notify_local_abort();

} // namespace hal_bridge
