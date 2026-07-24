# Module LLM セットアップ手順

StackChan で Module LLM Kit を使うための初期セットアップ手順です。

---

## 必要なもの

- PC（Windows / macOS / Linux）
- **データ転送対応の USB-C ケーブル**（充電専用不可）
- 自動セットアップを使う場合: `adb`
- 手動セットアップを使う場合: シリアルターミナル（例: [Tera Term](https://ttssh2.osdn.jp/)）
- **LAN ケーブル**（Module LLM Kit の Ethernet ポートをインターネットに接続するため）

---

## 推奨 — 自動セットアップ

通常はこちらを使います。Module LLM の USB-C ポートをPCへ接続し、Ethernetでインターネットへ接続してから、リポジトリのルートで実行してください。

```bash
./scripts/provision_module_llm.sh
```

このスクリプトが、StackFlow aptリポジトリ登録、必要パッケージ、モデル、Open JTalk、tohoku-f01 neutral voice、`/opt/stackchan/openjtalk_tts.sh` の配置まで行います。

以降のStep 1〜Step 6は、スクリプトを使わずに手動でセットアップする場合の手順です。

---

## Step 1 — 接続

1. Module LLM Kit の **Ethernet ポートに LAN ケーブル**を接続してインターネットに繋ぐ
2. Module LLM 本体の **USB-C ポート**（CoreS3 側ではなく Module LLM 側）に USB-C ケーブルで PC と接続する
3. デバイスマネージャーで **CH340** の COM ポート番号を確認する

---

## Step 2 — Tera Term で接続

1. Tera Term を起動
2. **シリアル接続** を選択、COM ポートを先ほど確認した番号に設定
3. ボーレート：**115200**、その他はデフォルトで接続
4. Enter を押すとログインプロンプトが表示される
5. 以下の認証情報でログイン

| 項目 | 値 |
|---|---|
| ユーザー名 | `root` |
| パスワード | `123456` |

```
root@m5stack-LLM:~#
```

---

## Step 3 — apt リポジトリを登録

以下を **1行ずつ**貼り付けて実行してください。

```bash
wget -qO - https://repo.llm.m5stack.com/m5stack-apt-repo/key/StackFlow.gpg | gpg --dearmor -o /etc/apt/keyrings/StackFlow.gpg
```

```bash
echo 'deb [arch=arm64 signed-by=/etc/apt/keyrings/StackFlow.gpg] https://repo.llm.m5stack.com/m5stack-apt-repo jammy ax630c' > /etc/apt/sources.list.d/StackFlow.list
```

```bash
apt update
```

`Reading package lists... Done` が表示されれば成功です。

> **オプション：システムを最新状態にする**  
> 不要な警告を減らしたい場合や、OS パッケージを最新にしておきたい場合は以下を実行してください。時間がかかります。
> ```bash
> apt install -y apt-utils
> apt upgrade -y
> ```

---

## Step 4 — ライブラリをインストール

```bash
apt install -y lib-llm llm-sys llm-audio llm-whisper llm-llm llm-melotts llm-vad
```

> `llm-vad` は音声区間検出（VAD）に必要です。設定画面で VAD を有効にする場合は必須です。

### Open JTalk / tohoku voice（日本語TTS推奨）

このブランチでは、日本語TTSにOpen JTalk + tohoku-f01 neutral voiceを優先して使います。
初回セットアップ時だけインターネット接続が必要ですが、発話時はModule LLM内だけで完結します。

```bash
apt install -y open-jtalk open-jtalk-mecab-naist-jdic alsa-utils
mkdir -p /opt/stackchan/voices
wget -O /opt/stackchan/voices/tohoku-f01-neutral.htsvoice \
  https://raw.githubusercontent.com/icn-lab/htsvoice-tohoku-f01/master/tohoku-f01-neutral.htsvoice
wget -O /opt/stackchan/voices/tohoku-f01-COPYRIGHT.txt \
  https://raw.githubusercontent.com/icn-lab/htsvoice-tohoku-f01/master/COPYRIGHT.txt
```

---

## Step 5 — モデルをインストール

### 必須モデル（日本語構成）

```bash
apt install -y llm-model-whisper-tiny llm-model-qwen3-0.6b-ax630c llm-model-melotts-ja-jp llm-model-silero-vad
```

### オプション：TTS 言語モデル

設定画面で TTS の言語を切り替える場合は、使用したい言語のモデルを追加インストールしてください。

```bash
# 中国語 TTS
apt install -y llm-model-melotts-zh-cn

# 英語 TTS
apt install -y llm-model-melotts-en-default llm-model-melotts-en-us
```

モデルファイルは合計 1GB 前後あるため、**数分〜十数分かかります**。プロンプトが返ってくるまで待ってください。

---

## Step 6 — 再起動

```bash
reboot
```

---

## Step 7 — CoreS3 に取り付けて起動

再起動完了後、USB-C ケーブルを外してから Module LLM を CoreS3 に取り付け、電源を入れます。  
起動時に Module LLM が自動で検出され、Whisper → Qwen3 → Open JTalk（未セットアップ時はMeloTTS）のパイプラインが構築されます。

---

## 設定画面（Module LLM Settings）

SETUP アプリ → **Module LLM** → **Settings** から以下の設定を変更できます。

| 設定項目 | 内容 |
|---|---|
| Thinking | LLM の思考モード（ON にすると応答が遅くなる） |
| VAD | 音声区間検出。ONにするとノイズを拾いにくくなる（要 `llm-vad` + `llm-model-silero-vad`）|
| TTS Language | 音声合成の言語（Japanese / Chinese / English）。対応モデルのインストールが必要 |

設定変更後は **Confirm** を押すと保存され、自動再起動で反映されます。
