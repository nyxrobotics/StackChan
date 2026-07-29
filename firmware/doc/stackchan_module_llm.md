# StackChan Hybrid Conversation Backend

> **Current setup procedure:** Module LLMの環境構築・検証・復旧は、リポジトリ
> ルートの[LLM_Module_Setup.md](../../LLM_Module_Setup.md)を参照してください。
> この文書のパッケージコマンドは初期設計時の参考情報であり、現在必要なVAD
> PCM bridge、Open JTalk helper、watchdog、S3制御の推論サービス開始・停止
> までは構築しません。

> **対象ハードウェア:** M5Stack CoreS3 + Module LLM  
> **対象ファームウェア:** [StackChan](https://github.com/m5stack/StackChan) (official)  
> **仕様バージョン:** draft-1

オンライン（Xiaozhi）とローカル（Module LLM）を自動切替するハイブリッド会話バックエンドです。  
ネットワーク断時は自動でローカル推論（VAD → Whisper → Qwen3 → Open JTalk）にフォールバックします。

```
Xiaozhi Online  →  Module LLM Local  →  Static Fallback
   (優先)              (オフライン)           (最終手段)
```

---

## 目次

1. [必要なもの](#1-必要なもの)
2. [ディレクトリ構成](#2-ディレクトリ構成)
3. [ファイルの配置](#3-ファイルの配置)
4. [CMakeLists.txt の更新](#4-cmakeliststxt-の更新)
5. [AI.AGENT への統合](#5-aiagent-への統合)
6. [TODO 箇所の実装](#6-todo-箇所の実装)
7. [NVS パーティションの設定](#7-nvs-パーティションの設定)
8. [ビルドと書き込み](#8-ビルドと書き込み)
9. [動作確認](#9-動作確認)
10. [AgentConfig サーバーの設定](#10-agentconfig-サーバーの設定)
11. [トラブルシューティング](#11-トラブルシューティング)
12. [開発フェーズ対応表](#12-開発フェーズ対応表)

1. [必要なもの](#1-必要なもの)
2. [**【先に実施】Module LLM へのパッケージインストール**](#2-先に実施-module-llm-へのパッケージインストール)
3. [ディレクトリ構成](#3-ディレクトリ構成)
4. [ファイルの配置](#4-ファイルの配置)
5. [CMakeLists.txt の更新](#5-cmakeliststxt-の更新)
6. [AI.AGENT への統合](#6-aiagent-への統合)
7. [TODO 箇所の実装](#7-todo-箇所の実装)
8. [NVS パーティションの設定](#8-nvs-パーティションの設定)
9. [ビルドと書き込み](#9-ビルドと書き込み)
10. [動作確認](#10-動作確認)
11. [AgentConfig サーバーの設定](#11-agentconfig-サーバーの設定)
12. [トラブルシューティング](#12-トラブルシューティング)
13. [開発フェーズ対応表](#13-開発フェーズ対応表)


## 1. 必要なもの

### ハードウェア

| 機器 | 型番 | 備考 |
|---|---|---|
| メインボード | M5Stack CoreS3 | ESP32-S3搭載 |
| AIモジュール | M5Stack Module LLM | AX630C NPU搭載 |
| 接続 | Module LLM を CoreS3 のモジュールスロットへ装着 | UART2使用 (TX=17, RX=18) |

### ソフトウェア

| ツール | バージョン | インストール |
|---|---|---|
| ESP-IDF | v5.1 以上 | [公式インストール手順](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/) |
| Python | 3.8 以上 | ESP-IDF に同梱 |
| StackChan firmware | 最新 main ブランチ | `git clone https://github.com/m5stack/StackChan` |

---


---

## 2. 【先に実施】Module LLM へのパッケージインストール

> ⚠️ **これはファームウェアを書き込む前に行う必要があります。**  
> Module LLM は Linux (Ubuntu 22.04 arm64) で動いており、モデルと機能モジュールを **Module LLM 側の apt で個別にインストール** する必要があります。  
> インストールされていないとファームウェア起動時のモデルロードが全て失敗します。

---

### 2-1. Module LLM のターミナルに接続する

以下のいずれかの方法で Module LLM のシェルに入ります。

**方法A: ADB (USB Type-C)**

```bash
# Module LLM の Type-C ポートを PC に接続してから:
adb shell
```

**方法B: UART (デバッグボード経由)**

デバッグボードのシリアルポートに Putty などで接続します。  
設定: `115200 bps / 8N1`  
ログイン: ユーザー `root` / パスワード `123456`

**方法C: SSH (イーサネット経由)**

```bash
# まず ADB か UART で IP アドレスを確認する
ip addr

# SSH で接続 (ユーザー: root / パスワード: 123456)
ssh root@<Module_LLM_の_IP>
```

---

### 2-2. apt ソースの登録 (初回のみ)

```bash
# GPG キーを追加
wget -qO /etc/apt/keyrings/StackFlow.gpg \
  https://repo.llm.m5stack.com/m5stack-apt-repo/key/StackFlow.gpg

# M5Stack リポジトリを追加
echo 'deb [arch=arm64 signed-by=/etc/apt/keyrings/StackFlow.gpg] \
  https://repo.llm.m5stack.com/m5stack-apt-repo jammy ax630c' \
  > /etc/apt/sources.list.d/StackFlow.list

# パッケージリストを更新
apt update
```

---

### 2-3. 必要なパッケージを一括インストール

#### ベースライブラリ・機能モジュール

```bash
apt install lib-llm      # 実行環境ベースライブラリ
apt install llm-sys      # StackFlow 基盤
apt install llm-audio    # サウンドカード管理
apt install llm-whisper  # Whisper ASR 機能モジュール
apt install llm-llm      # LLM テキスト生成機能モジュール
apt install llm-melotts  # MeloTTS TTS 機能モジュール
```

#### モデルファイル (容量大・要時間)

```bash
apt install llm-model-whisper-tiny          # Whisper Tiny (~数十MB)
apt install llm-model-qwen3-0.6b-ax630c     # Qwen3-0.6B (~数百MB)
apt install llm-model-melotts-ja-jp         # MeloTTS 日本語モデル
```

> **注意:** モデルはストレージを大量に消費します。  
> `df -h` で空き容量を確認してからインストールしてください。

---

### 2-4. インストール確認

```bash
# インストール済みパッケージを確認
apt list --installed | grep llm

# StackFlow サービスが動いているか確認
ps aux | grep stackflow
```

以下が全て表示されれば OK です:

```
lib-llm
llm-sys
llm-audio
llm-whisper
llm-llm
llm-melotts
llm-model-whisper-tiny
llm-model-qwen3-0.6b-ax630c
llm-model-melotts-ja-jp
```

---

### 2-5. Module LLM を再起動

インストール完了後、Module LLM を再起動してサービスを有効化します。

```bash
reboot
```

> ここまで完了したら、以下のファームウェアセットアップ手順に進んでください。

## 3. ディレクトリ構成

統合後のファームウェアディレクトリ構成:

```
StackChan/
└── firmware/
    └── main/
        ├── apps/
        │   └── app_ai_agent/
        │       └── app_ai_agent.cpp   ← 統合ポイント
        ├── hal/
        │   └── board/
        │       └── stackchan.cc
        └── conversation/              ← このディレクトリを追加
            ├── conversation_backend.h
            ├── conversation_health.h
            ├── conversation_manager.h
            ├── conversation_manager.cpp
            ├── xiaozhi_backend.h
            ├── xiaozhi_backend.cpp
            ├── module_llm_backend.h
            ├── module_llm_backend.cpp
            ├── module_llm_client.h
            ├── module_llm_client.cpp
            ├── static_fallback_backend.h
            ├── static_fallback_backend.cpp
            ├── agent_config_provider.h
            ├── agent_config_provider.cpp
            ├── agent_config_store.h
            └── agent_config_store.cpp
```

---

## 4. ファイルの配置

```bash
# StackChan リポジトリに移動
cd StackChan/firmware/main

# conversation/ ディレクトリを作成してファイルをコピー
mkdir -p conversation
cp /path/to/downloaded/conversation/*.h   conversation/
cp /path/to/downloaded/conversation/*.cpp conversation/
```

---

## 5. CMakeLists.txt の更新

`firmware/main/CMakeLists.txt` に `conversation/` ディレクトリを追加します。

```cmake
# 既存の SRCS リストに追記
idf_component_register(
    SRCS
        # ... 既存のソース ...
        "conversation/conversation_manager.cpp"
        "conversation/xiaozhi_backend.cpp"
        "conversation/module_llm_backend.cpp"
        "conversation/module_llm_client.cpp"
        "conversation/static_fallback_backend.cpp"
        "conversation/agent_config_provider.cpp"
        "conversation/agent_config_store.cpp"
    INCLUDE_DIRS
        "."
        "conversation"   # ← 追加
    REQUIRES
        # 既存の依存関係に追記
        nvs_flash
        esp_http_client
        driver          # UART ドライバ
        json            # cJSON
)
```

---

## 6. AI.AGENT への統合

`firmware/main/apps/app_ai_agent/app_ai_agent.cpp` を編集します。

### 5-1. インクルードの追加

```cpp
// app_ai_agent.cpp の先頭に追加
#include "conversation/conversation_manager.h"
```

### 5-2. ConversationManager のインスタンス化

クラスメンバーに追加:

```cpp
class AppAiAgent {
    // ... 既存メンバー ...
    std::unique_ptr<ConversationManager> conv_;
};
```

### 5-3. 起動時の初期化

`onStart()` または `AI.AGENT open` ハンドラ内:

```cpp
void AppAiAgent::onStart() {
    // 既存処理 ...

    // ConversationManager を初期化して起動
    conv_ = std::make_unique<ConversationManager>(ConversationMode::Auto);
    conv_->start();
}
```

### 5-4. Wi-Fi / Xiaozhi イベントのフック

既存のコールバックから ConversationManager に通知します:

```cpp
// Wi-Fi 接続時
wifi_event_handler(...) {
    if (event_id == WIFI_EVENT_STA_CONNECTED)    conv_->onNetworkConnected();
    if (event_id == WIFI_EVENT_STA_DISCONNECTED) conv_->onNetworkDisconnected();
}

// Xiaozhi WebSocket イベント
xiaozhi_on_connected()    { conv_->onXiaozhiConnected(); }
xiaozhi_on_disconnected() { conv_->onXiaozhiDisconnected(); }
xiaozhi_on_error()        { conv_->onXiaozhiError(); }
```

### 5-5. 会話ターンの委譲

既存のターン処理を ConversationManager 経由に変更:

```cpp
// ターン開始
void AppAiAgent::onVoiceRecordStart() {
    conv_->onTurnStart();
}

// 音声入力を渡す
void AppAiAgent::onAudioData(const uint8_t* pcm, size_t len) {
    conv_->processAudio(pcm, len);
}

// ターン終了
void AppAiAgent::onVoiceRecordEnd() {
    conv_->onTurnEnd();
}
```

---

## 7. TODO 箇所の実装

実装ファイル内に `// TODO:` コメントが残っている箇所を、既存ファームウェアの API に合わせて埋めてください。

### xiaozhi_backend.cpp

```cpp
// start() — Xiaozhi WebSocket が開いていることを確認
void XiaozhiBackend::start() {
    // 例: XiaozhiClient::getInstance().ensureConnected();
}

// processAudio() — 既存の音声送信処理を委譲
void XiaozhiBackend::processAudio(const uint8_t* pcm, size_t len) {
    // 例: XiaozhiClient::getInstance().sendAudio(pcm, len);
}
```

### module_llm_client.cpp — 音声データ転送

現状は `audio_len` のみ送信しています。  
StackFlow の仕様に応じて base64 エンコードまたは UART 直接ストリーミングに変更してください:

```cpp
// runAsr() 内 — base64 エンコードする場合
#include <mbedtls/base64.h>

size_t b64Len = 0;
mbedtls_base64_encode(nullptr, 0, &b64Len, pcm, len);
std::string b64(b64Len, '\0');
mbedtls_base64_encode(
    reinterpret_cast<uint8_t*>(b64.data()), b64Len, &b64Len, pcm, len);
cJSON_AddStringToObject(params, "audio_b64", b64.c_str());
```

### module_llm_backend.cpp — 音声再生

```cpp
void ModuleLLMBackend::playback(const uint8_t* pcm, size_t len) {
    // 例: HAL の音声出力 API を呼ぶ
    // AudioOutput::getInstance().play(pcm, len, /*sampleRate=*/22050);
}
```

### agent_config_provider.cpp — エンドポイントの設定

```cpp
// AgentConfigProvider::AgentConfigProvider() のデフォルト値を
// NVS から読んだサーバーURLに変更するか、起動時に setEndpoint() を呼ぶ:

// app_ai_agent.cpp 側:
std::string serverUrl = nvs_get_string("server_url"); // 既存NVS読み取り
conv_->configProvider().setEndpoint(serverUrl + "/api/agent/config");
```

---

## 8. NVS パーティションの設定

AgentConfig の NVS 保存に専用ネームスペース `agentcfg` を使用します。  
既存の `nvs_flash_init()` が完了していれば追加設定は不要です。

`partitions.csv` に NVS パーティションが存在することを確認:

```csv
# Name,   Type, SubType, Offset,  Size
nvs,      data, nvs,     0x9000,  0x6000
...
```

---

## 9. ビルドと書き込み

```bash
cd StackChan/firmware

# ターゲットを CoreS3 (ESP32-S3) に設定
idf.py set-target esp32s3

# ビルド
idf.py build

# 書き込み（ポートは環境に合わせて変更）
idf.py -p /dev/ttyUSB0 flash

# シリアルモニタ
idf.py -p /dev/ttyUSB0 monitor
```

---

## 10. 動作確認

起動後のシリアルログで以下を順番に確認してください。

### Module LLM 接続確認

```
I (xxxx) ModLLMClient: Module LLM connected
I (xxxx) ModLLMClient: Whisper loaded
I (xxxx) ModLLMClient: Qwen3 loaded
I (xxxx) ModLLMClient: MeloTTS loaded
I (xxxx) ModLLMClient: Pipeline ready
```

### AgentConfig 取得確認

オンライン時（NVS 保存）:
```
I (xxxx) AgentCfgProv: parsed AgentConfig: lang=ja char=...
I (xxxx) AgentCfgStore: config saved to NVS
```

オフライン時（NVS 読込）:
```
I (xxxx) AgentCfgStore: config loaded from NVS (lang=ja)
```

### バックエンド選択確認

```
I (xxxx) ConvMgr: Activated backend: 0   ← 0=Xiaozhi, 1=ModuleLLM, 2=Static
```

### ネットワーク断テスト

Wi-Fi を切断してターンを開始し、以下のログが出ることを確認:

```
W (xxxx) ConvMgr: Network disconnected → onlineReady=false
I (xxxx) ConvMgr: Activated backend: 1   ← ModuleLLM に切替
```

---

## 11. AgentConfig サーバーの設定

StackChan サーバーの AgentConfig エンドポイントは以下の JSON を返す必要があります:

```json
{
  "language": "ja",
  "character": "あなたはStackChanです。",
  "memory": "",
  "tts": {
    "model_id": "melotts-ja-jp",
    "language": "ja_JP",
    "speed": 1.0,
    "pitch": 1.0
  }
}
```

サーバー実装の参考: [`server/internal/model/xiaozhi/agent.go`](https://github.com/m5stack/StackChan/blob/main/server/internal/model/xiaozhi/agent.go)

---

## 12. トラブルシューティング

### Module LLM に接続できない

```
W (xxxx) ModLLMClient: Module LLM did not respond to ping
```

確認項目:
- Module LLM が CoreS3 のモジュールスロットに正しく装着されているか
- `kRxPin=18`, `kTxPin=17` が実際の配線と一致しているか (`module_llm_client.h` で変更可)
- Module LLM の電源が入っているか（起動に数秒かかる場合あり）

### モデルロードがタイムアウトする

```
W (xxxx) ModLLMClient: waitForAck(llm.model.load): timeout
```

`loadModelsAndPipeline()` のタイムアウト値を延長してください（特に Qwen3 は初回起動時に時間がかかります）:

```cpp
// module_llm_client.cpp — loadQwen() 内
bool ok = stackflowSend(s) && waitForAck(kMethodLoadModel, 120000); // 60s → 120s
```

### NVS 保存が失敗する

```
E (xxxx) AgentCfgStore: save failed: ESP_ERR_NVS_NOT_ENOUGH_SPACE
```

`partitions.csv` の NVS パーティションサイズを増やしてください（最低 `0x6000` 推奨）。

### オフライン時に StaticFallback になってしまう

Module LLM は接続済みだが `localPipelineReady=false` の可能性があります。  
シリアルログで `Pipeline ready` が出ているか確認してください。  
出ていない場合はモデルロードのエラーログを追ってください。

---

## 13. 開発フェーズ対応表

仕様書 Section 15 のフェーズと実装ファイルの対応:

| フェーズ | 内容 | 主な実装ファイル |
|---|---|---|
| Phase 1 | Xiaozhi 抽象化 | `conversation_backend.h`, `conversation_manager.*`, `xiaozhi_backend.*` |
| Phase 2 | Module LLM 検出 | `module_llm_client.*` — `connect()` |
| Phase 3 | モデルロード | `module_llm_client.*` — `loadModelsAndPipeline()` |
| Phase 4 | AgentConfig キャッシュ | `agent_config_provider.*`, `agent_config_store.*` |
| Phase 5 | ローカル会話 | `module_llm_backend.*`, `module_llm_client.*` — `runAsr/Llm/Tts()` |
| Phase 6 | フォールバック | `conversation_manager.cpp` — `onBackendFailure()`, `static_fallback_backend.*` |

---

## ライセンス・参考リンク

- [StackChan firmware](https://github.com/m5stack/StackChan)
- [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32)
- [Module LLM ドキュメント](https://docs.m5stack.com/ja/module/Module-LLM)
- [StackFlow API](https://docs.m5stack.com/en/stackflow/module_llm/api)
- [MeloTTS Japanese モデル](https://docs.m5stack.com/ja/stackflow/models/melotts-japanese)
- [Qwen3-0.6B モデル](https://docs.m5stack.com/ja/stackflow/models/qwen3-0.6b)
- [AI_StackChan_Ex (参考実装)](https://github.com/ronron-gh/AI_StackChan_Ex)
