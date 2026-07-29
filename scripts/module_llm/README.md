# Module LLM provisioning

このディレクトリには、StackChanのローカル会話環境をModule LLM上へ構築する
スクリプト一式が入っています。通常の入口は、リポジトリルートで実行する
ホスト側ラッパーです。

```bash
./scripts/provision_module_llm.sh all --no-reboot
./scripts/provision_module_llm.sh verify --no-reboot
```

ラッパーはこのディレクトリを一式まとめてADB転送し、Module LLM上で必要な
スクリプトを実行します。一部のファイルだけをModuleへコピーして直接実行する
運用は想定していません。詳しい初期構築手順は
[LLM_Module_Setup.md](../../LLM_Module_Setup.md)を参照してください。

## Feature targets

| Target | Module-side script | Installed function |
|---|---|---|
| `repo` | `setup_repo.sh` | M5Stack StackFlow apt repository |
| `runtime` | `setup_runtime.sh` | `lib-llm`, `llm-sys`, `llm-audio`, PCM probe, S3 runtime control |
| `whisper` | `setup_whisper.sh` | Whisper function and tiny model |
| `qwen3` | `setup_qwen3.sh` | Qwen3 0.6B function, model, tokenizer link |
| `vad` | `setup_vad.sh` | Silero VAD, PCM bridge, systemd service |
| `melotts` | `setup_melotts.sh` | Non-Japanese/fallback TTS models |
| `openjtalk` | `setup_openjtalk.sh` | Open JTalk, dictionary, tohoku-f01 voice |
| `watchdog` | `setup_llm_sys_watchdog.sh` | `llm-sys` health watchdog and recovery |
| `verify` | `verify_setup.sh` | Packages, files, model compatibility, services |
| `all` | `setup_llm_module.sh` | Every target above, followed by verification |

Optional MeloTTS languages are selected with `--with-zh-tts`,
`--with-en-tts`, or `--with-all-tts` on the `all`, `melotts`, and `verify`
targets.

## Installed StackChan files

| Module path | Purpose |
|---|---|
| `/opt/stackchan/openjtalk_tts.sh` | Offline Japanese synthesis and playback |
| `/opt/stackchan/voices/tohoku-f01-neutral.htsvoice` | Japanese voice |
| `/opt/stackchan/stackflow_pcm_ready.py` | Real `sys.pcm` frame probe |
| `/opt/stackchan/stackchan_vad_pcm_bridge.py` | RAM-only VAD segment handoff to Whisper |
| `/opt/stackchan/stackchan_llm_sys_watchdog.py` | StackFlow/UART health monitor |
| `/opt/stackchan/stackchan_local_runtime.sh` | S3-controlled inference service start/stop |
| `/etc/systemd/system/stackchan-vad-pcm-bridge.service` | VAD bridge service |
| `/etc/systemd/system/stackchan-llm-sys-watchdog.service` | Watchdog service |
| `/etc/systemd/system/llm-sys.service.d/stackchan-watchdog.conf` | Bounded `llm-sys` stop timeout |

The VAD bridge keeps normal audio exchange in bounded RAM buffers. It writes a
single diagnostic WAV under `/tmp` only when
`/run/stackchan-vad-pcm-bridge.debug` is explicitly created. Both locations are
tmpfs on the Module LLM, and disabling debug removes the diagnostic file.

The watchdog uses a dedicated local-turn marker to treat Qwen inference and
OpenJTalk playback as legitimate UART work without confusing pipeline setup
with an active conversation. TCP control failures are still recovered
immediately, and the UART timeout resumes after the turn ends. Deferral is
bounded, so a stale local-turn marker cannot suppress UART recovery forever.
The CoreS3 separately rotates lightweight `taskinfo` probes across the live
VAD, Whisper, LLM, and TTS work IDs. A service restart that discards
StackFlow tasks therefore causes a full pipeline rebuild without invoking a
shell command in the ASR delivery path.

`llm-sys` and the watchdog form the always-on control plane. Audio, VAD,
Whisper, Qwen3, MeloTTS, and the VAD bridge form an inference plane that is
disabled at Linux boot. CoreS3 starts or stops that plane over the existing
UART `sys.bashexec` channel. Desired state is recorded only in
`/run/stackchan-local-runtime.active`, so it causes no persistent storage
writes. The watchdog restores the inference plane after an `llm-sys` recovery
only while that marker says CoreS3 still needs it.

## Setup behavior

- Every target is idempotent and can be rerun to repair its own feature.
- Packages are installed or upgraded only when a setup command is explicitly
  run. No automatic package updater is installed.
- Downloads and ADB operations have bounded timeouts.
- Files are installed atomically. The control plane is enabled and started;
  inference services are disabled and stopped until CoreS3 requests them.
- Reboot is not required. The default is `--no-reboot` because `reboot` and
  `adb reboot` can leave some Module LLM revisions powered off.
- Internet access is needed for initial package and voice installation. Runtime
  ASR, LLM, and TTS remain local and work without internet access.

## Verification

The verifier is read-only with respect to the installed configuration. It
fails when a required package is missing or outdated, a helper is absent, the
Qwen tokenizer or Silero mode configuration is invalid, the tohoku voice is
not selected, a control service is not enabled and active, or an inference
service is enabled at boot. It also rejects a partial inference-service state.

```bash
./scripts/provision_module_llm.sh verify --no-reboot

adb shell systemctl status stackchan-vad-pcm-bridge.service
adb shell systemctl status stackchan-llm-sys-watchdog.service
adb shell journalctl -u stackchan-llm-sys-watchdog.service -n 100 --no-pager
adb shell /opt/stackchan/stackchan_local_runtime.sh status
```

For a direct lifecycle check, the same helper accepts `start` and `stop`.
Normal operation sends those commands from CoreS3 rather than ADB.

For development, validate the source bundle before transferring it:

```bash
bash -n scripts/provision_module_llm.sh scripts/module_llm/*.sh
python3 -m py_compile scripts/module_llm/*.py
python3 -m unittest discover -s scripts/module_llm/tests -v
git diff --check
```
