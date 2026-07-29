# Module LLMローカル会話環境の構築

M5Stack CoreS3 + Module LLMで、音声認識、応答生成、日本語音声合成を
インターネットなしで実行するための手順です。インターネット接続が必要なのは
Module LLMへパッケージとモデルを導入する初回セットアップ時だけです。

実行時の標準パイプラインは次の構成です。

```text
Module microphone
  -> Silero VAD
  -> RAM上のVAD区間PCM bridge
  -> Whisper tiny
  -> Qwen3 0.6B
  -> Open JTalk + tohoku-f01 neutral voice
```

VAD有効時、Whisperが受け取る音声はVADで確定した区間だけです。Whisperが
`sys.pcm`を直接読む経路とは併用しません。MeloTTSは日本語Open JTalkが利用
できない場合と、追加言語を選択した場合のフォールバックです。

通常起動では`llm-sys`と監視サービスだけが待機し、音声、VAD、Whisper、
Qwen3、MeloTTS、PCM bridgeのサービスは起動しません。必要になった時点で
CoreS3がUART経由で一括起動し、不要になれば一括停止します。

## 1. 必要なもの

- M5Stack CoreS3とModule LLM
- データ転送対応USB-Cケーブル
- Module LLM初期構築用の有線インターネット接続
- BashとADBを実行できるPC
- CoreS3ファームウェア用ESP-IDF v5.5.4
- Module LLMとCoreS3を安定して動かせる電源

Ubuntu系LinuxでADBがない場合は、次のように導入できます。

```bash
sudo apt update
sudo apt install adb
```

macOSではHomebrewの`android-platform-tools`、WindowsではAndroid SDK
Platform ToolsまたはWSLを利用できます。

## 2. Module LLMを接続する

1. Module LLMのEthernetポートをインターネットへ接続します。
2. Module LLM側のUSB-CポートをPCへ接続します。
3. Module LLMの電源スイッチがONであることを確認します。
4. リポジトリルートでADB接続を確認します。

```bash
adb devices -l
```

`device`状態のModule LLMが1台表示されれば準備完了です。複数のADB機器が
ある場合は`ANDROID_SERIAL=<serial>`を指定してください。

Linuxで`no permissions`になる場合は、udevルールを設定するか、ADBサーバー
の権限を直してから再実行します。セットアップスクリプトがsudoパスワードを
保存することはありません。

## 3. 初期環境を一括構築する

リポジトリルートから、次の1コマンドを実行します。

```bash
./scripts/provision_module_llm.sh all --no-reboot
```

ホスト側スクリプトは、`scripts/module_llm/`を一時ディレクトリへ一式転送し、
Module LLM上で機能別セットアップを依存順に実行します。

1. StackFlow apt repository
2. `lib-llm`, `llm-sys`, `llm-audio`とS3用runtime制御helper
3. Whisper tiny
4. Qwen3 0.6Bとtokenizer互換リンク
5. Silero VADとVAD PCM bridge
6. MeloTTS日本語フォールバック
7. Open JTalk、辞書、tohoku-f01 neutral voice
8. `llm-sys`無応答監視サービス
9. 全パッケージ、補助ファイル、systemdサービスの検証

スクリプトは再実行可能です。既に正しいものは維持し、不足・旧版・壊れた
補助ファイルだけを修復します。パッケージ更新はこのコマンドを明示的に実行
したときだけ行われ、自動更新サービスは導入しません。

`--upgrade`を付けた場合だけModule全体の`apt upgrade`も実行します。

```bash
./scripts/provision_module_llm.sh all --upgrade --no-reboot
```

## 4. 構築結果を検証する

一括セットアップの最後にも実行されますが、いつでも独立して再検証できます。

```bash
./scripts/provision_module_llm.sh verify --no-reboot
```

検証対象は次のとおりです。

- 必須パッケージとモデルがインストール済みで、apt候補版と一致すること
- Open JTalkがtohoku-f01 neutral voiceを選択していること
- Qwen3 tokenizer互換リンクとSilero VAD設定が有効なこと
- PCM probe、VAD PCM bridge、watchdog、runtime制御helperが実行可能なこと
- `llm-sys`とwatchdogが`enabled`かつ`active`であること
- 推論系systemdサービスが起動時`disabled`で、全起動または全停止の一貫した
  状態にあること

セットアップ完了時は制御サービスだけが動作し、推論系は停止しています。
通常はLinuxの再起動を必要としません。一部のModule LLMでは`reboot`や
`adb reboot`を実行すると電源が切れたまま戻らないため、完全な電源再投入が
必要な場合は本体の物理スイッチをOFF/ONしてください。

## 5. CoreS3ファームウェアを書き込む

CoreS3をPCへ接続し、ESP-IDF v5.5.4を有効にしてビルド・書き込みします。
ポート名は環境に合わせて変更してください。

```bash
cd firmware
source ~/esp/esp-idf-v5.5.4/export.sh
idf.py build
idf.py -p /dev/ttyACM0 flash
```

ファームウェアだけを更新する場合は`app-flash`も利用できます。

```bash
idf.py -p /dev/ttyACM0 -b 115200 app-flash
```

このファームウェアは、デバッグ目的でAI Agentを強制起動しません。通常は顔を
タップしてAI Agentを開きます。SETUP内の`Start AI Agent on boot`をユーザーが
有効にしている場合だけ、保存済み設定に従って自動起動します。

ローカル会話を固定して確認する場合は、SETUPの`LLM Mode`で
`Local Only (Module LLM)`を選び、`Module LLM Settings`でVADを有効にします。
同じ画面の`Open JTalk volume`では、日本語音声の出力ゲインを`-60`から
`0 dB`まで調整できます。既定値は静かな連続試験向けの`-36 dB`です。

`Module LLM Settings`の`Auto: fast fallback`は既定でOFFです。OFFの
`Power Save`動作では、Autoモード中もオンライン利用中は推論系を停止し、
オンライン障害時だけ起動します。オンライン復帰後はオンラインbackendへ
戻して推論系を停止します。ONにすると、オンライン接続後にローカルpipelineも
予熱して、消費電力より切替時間を優先します。`Online Only`では常に推論系を
停止し、`Local Only`ではAI Agent開始時に起動します。

## 6. オフライン動作を確認する

初期構築とファームウェア書き込みが終わったら、Module LLMのEthernetを外して
構いません。Module LLMとCoreS3を起動し、顔をタップしてAI Agentを開きます。

正常時の代表的なCoreS3ログは次のとおりです。

```text
ModLLMClient: Pipeline ready (... tts=openjtalk)
ModLLMBackend: VAD: SPEECH
ModLLMBackend: VAD: silence
ModLLMBackend: ASR: ...
ModLLMBackend: LLM raw response: ...
ModLLMBackend: OpenJTalk TTS result: ... STACKCHAN_OPENJTALK_DONE
ModLLMClient: VAD PCM bridge resume confirmed
```

VAD終端から口パクを開始し、Open JTalk再生完了後にマイク待受けへ戻ります。
顔タップによる中断後も、次のターンでLLMとTTSを再開します。

## 7. 機能単位で修復する

すべてPC側の同じ入口から実行します。Module LLMへ個別スクリプトを手作業で
コピーする必要はありません。

| Target | 内容 |
|---|---|
| `repo` | StackFlow apt repositoryと`apt update` |
| `runtime` | `llm-sys`, audio、PCM probe |
| `whisper` | Whisper機能とtinyモデル |
| `qwen3` | Qwen3機能、0.6Bモデル、tokenizerリンク |
| `vad` | Silero VAD、設定補完、PCM bridge |
| `melotts` | MeloTTSと追加言語モデル |
| `openjtalk` | Open JTalk、辞書、tohoku voice、TTS helper |
| `watchdog` | `llm-sys`監視と自動復旧 |
| `verify` | 環境全体の読み取り検証 |

例:

```bash
./scripts/provision_module_llm.sh vad --no-reboot
./scripts/provision_module_llm.sh openjtalk --no-reboot
./scripts/provision_module_llm.sh watchdog --no-reboot
./scripts/provision_module_llm.sh melotts --with-en-tts --no-reboot
```

Module上の機能別スクリプト構成と配置先は
[scripts/module_llm/README.md](scripts/module_llm/README.md)にまとめています。

## 8. 状態確認と復旧

```bash
adb shell systemctl status llm-sys.service
adb shell systemctl status stackchan-vad-pcm-bridge.service
adb shell systemctl status stackchan-llm-sys-watchdog.service
adb shell journalctl -u stackchan-vad-pcm-bridge.service -n 100 --no-pager
adb shell journalctl -u stackchan-llm-sys-watchdog.service -n 100 --no-pager
adb shell /opt/stackchan/stackchan_llm_sys_watchdog.py --check
adb shell /opt/stackchan/stackchan_local_runtime.sh status
```

CoreS3はUARTの応答停止を検出するとStaticFallbackへ退避し、5秒後からModule
LLMへの再接続を試します。Module側watchdogはローカルStackFlow pingの連続失敗
またはUART RXだけが進む停止状態を検出し、`llm-sys`を復旧します。S3が
ローカル推論を要求中の場合だけ、`/run`上のruntime markerに従って推論系も
復旧します。停止中の推論系をwatchdogが勝手に起動することはありません。
Qwen推論からOpen JTalk再生までの会話処理中はUART応答が長時間止まり得るため、
専用のlocal-turnマーカーがある間はwatchdogのUART停止判定を保留します。
セットアップ時のVAD停止とは区別され、保留時間にも上限があるため、マーカーが
残留してもUART復旧が永久に抑止されることはありません。ローカル
StackFlow ping自体が失敗した場合は保留せず復旧します。CoreS3は待受中にVAD、
Whisper、LLM、TTSのwork IDを軽量な`taskinfo`で順番に確認します。VAD終端後の
5秒間はASR結果の配送を優先し、検査コマンドを送信しません。サービス再起動で
実行タスクが失われた場合は、
`llm-sys`だけが応答していても異常として検出し、パイプライン全体を再構築して
ModuleLLMバックエンドへ自動で戻ります。

VAD bridgeの通常のPCM受け渡しはRAM内で完結します。診断用WAVが必要な場合だけ
次を実行し、確認後に無効化してください。`/tmp`と`/run`はModule上のtmpfsです。

```bash
adb shell touch /run/stackchan-vad-pcm-bridge.debug
# 最新区間: /tmp/stackchan-vad-pcm-bridge/segment-latest.wav
adb shell rm -f /run/stackchan-vad-pcm-bridge.debug
```

### 無人の連続試験用ビルド

通常ファームは顔操作または保存済みの起動設定に従います。ハードウェア連続試験で
起動時の顔タッチだけを省略したい場合は、既定OFFの
`CONFIG_STACKCHAN_TEST_AUTO_START_AI_AGENT`を有効にします。

対話的に設定する場合は`idf.py menuconfig`の
`StackChan Development > Auto-start AI Agent for unattended tests`を選びます。
通常ビルドと設定を分離する場合は、次のローカルオーバーレイと専用build directoryを
使います。`sdkconfig.defaults.local`はGit管理対象外です。

```bash
cd firmware
printf '%s\n' 'CONFIG_STACKCHAN_TEST_AUTO_START_AI_AGENT=y' > sdkconfig.defaults.local
idf.py -B build-unattended \
  -D SDKCONFIG="$PWD/build-unattended/sdkconfig" build
idf.py -B build-unattended -p /dev/ttyACM0 app-flash
```

このオプションはランチャーだけを省略します。VAD、Whisper、Qwen、TTSの構成や
本番設定は変更しません。通常ファームでは無効のままにしてください。

## 9. セットアップに失敗する場合

- `adb devices -l`でModuleが`device`状態か確認する
- USBケーブルが充電専用でないか確認する
- ModuleのEthernetとDNSが利用できるか確認する
- `df -h /`で1GB以上の空き容量を確認する
- 失敗したtargetだけを再実行する
- `verify`の最初のエラーを直してから再検証する
- Linux再起動後に戻らない場合は物理電源スイッチをOFF/ONする

セットアップ処理にはADB・ダウンロード・モデル導入それぞれのタイムアウトが
あり、途中失敗時はエラーで停止します。Module上の一時転送ディレクトリは診断
のため保持され、成功時は自動削除されます。
