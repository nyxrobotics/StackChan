#!/usr/bin/env bash
set -euo pipefail

APP_NAME="stackchan-openjtalk"
TMP_ROOT="/tmp/${APP_NAME}"
AUDIO_DEVICE="${STACKCHAN_TTS_AUDIO_DEVICE:-plughw:0,1}"
VOICE_PATH="${STACKCHAN_OPENJTALK_VOICE:-}"
DICT_PATH="${STACKCHAN_OPENJTALK_DIC:-}"
SPEED="${STACKCHAN_OPENJTALK_SPEED:-1.05}"
PITCH_SHIFT="${STACKCHAN_OPENJTALK_PITCH_SHIFT:-0}"
ALPHA="${STACKCHAN_OPENJTALK_ALPHA:-0.55}"
VOLUME_GAIN="${STACKCHAN_OPENJTALK_VOLUME_GAIN:--36}"
SYNTH_TIMEOUT="${STACKCHAN_OPENJTALK_SYNTH_TIMEOUT:-120}"
PLAYBACK_TIMEOUT="${STACKCHAN_OPENJTALK_PLAYBACK_TIMEOUT:-300}"
TMP_FILES=()

cleanup_tmp_files() {
    if [ "${#TMP_FILES[@]}" -gt 0 ]; then
        rm -f -- "${TMP_FILES[@]}"
    fi
}

trap cleanup_tmp_files EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

usage() {
    cat <<'EOF'
Usage: openjtalk_tts.sh --check | --text TEXT

Environment:
  STACKCHAN_OPENJTALK_SYNTH_TIMEOUT     Synthesis timeout in seconds (default: 120).
  STACKCHAN_OPENJTALK_PLAYBACK_TIMEOUT  Playback timeout in seconds (default: 300).
  STACKCHAN_OPENJTALK_VOLUME_GAIN       Open JTalk output gain in dB (default: -36).
EOF
}

find_first_file() {
    for path in "$@"; do
        if [ -f "$path" ]; then
            echo "$path"
            return 0
        fi
    done
    return 1
}

find_first_dir() {
    for path in "$@"; do
        if [ -d "$path" ]; then
            echo "$path"
            return 0
        fi
    done
    return 1
}

resolve_paths() {
    if [ -z "$DICT_PATH" ]; then
        DICT_PATH="$(find_first_dir \
            /var/lib/mecab/dic/open-jtalk/naist-jdic \
            /usr/share/open_jtalk/open_jtalk_dic_utf_8-1.11 \
            /usr/share/open_jtalk/open_jtalk_dic_utf_8 \
            /usr/local/share/open_jtalk/open_jtalk_dic_utf_8-1.11 \
            /usr/local/share/open_jtalk/open_jtalk_dic_utf_8 \
            /usr/share/open_jtalk/dic || true)"
    fi

    if [ -z "$VOICE_PATH" ]; then
        VOICE_PATH="$(find_first_file \
            /opt/stackchan/voices/tohoku-f01-neutral.htsvoice \
            /usr/local/share/htsvoice/tohoku-f01-neutral.htsvoice \
            /usr/share/hts-voice/tohoku-f01/tohoku-f01-neutral.htsvoice \
            /usr/share/hts-voice/tohoku-f01-neutral.htsvoice \
            /usr/share/hts-voice/mei/mei_normal.htsvoice \
            /usr/share/hts-voice/nitech-jp-atr503-m001/nitech_jp_atr503_m001.htsvoice \
            /usr/share/hts-voice/nitech_jp_atr503_m001.htsvoice || true)"
    fi

    command -v open_jtalk >/dev/null 2>&1 || {
        echo "STACKCHAN_OPENJTALK_ERROR open_jtalk not found"
        return 1
    }
    command -v aplay >/dev/null 2>&1 || {
        echo "STACKCHAN_OPENJTALK_ERROR aplay not found"
        return 1
    }
    command -v timeout >/dev/null 2>&1 &&
        timeout --help 2>&1 | grep -- '--kill-after' >/dev/null || {
        echo "STACKCHAN_OPENJTALK_ERROR GNU timeout not found"
        return 1
    }
    [[ "$SYNTH_TIMEOUT" =~ ^[1-9][0-9]*$ ]] || {
        echo "STACKCHAN_OPENJTALK_ERROR invalid synthesis timeout"
        return 1
    }
    [[ "$PLAYBACK_TIMEOUT" =~ ^[1-9][0-9]*$ ]] || {
        echo "STACKCHAN_OPENJTALK_ERROR invalid playback timeout"
        return 1
    }
    [ -n "$DICT_PATH" ] && [ -d "$DICT_PATH" ] || {
        echo "STACKCHAN_OPENJTALK_ERROR dictionary not found"
        return 1
    }
    [ -n "$VOICE_PATH" ] && [ -f "$VOICE_PATH" ] || {
        echo "STACKCHAN_OPENJTALK_ERROR htsvoice not found"
        return 1
    }
}

stop_previous() {
    pkill -f "aplay.*${TMP_ROOT}" >/dev/null 2>&1 || true
    pkill -f "open_jtalk.*${TMP_ROOT}" >/dev/null 2>&1 || true
}

speak_text() {
    local text="$1"
    [ -n "$text" ] || {
        echo "STACKCHAN_OPENJTALK_ERROR empty text"
        return 1
    }

    resolve_paths
    stop_previous
    mkdir -p "$TMP_ROOT"

    local txt wav play_status
    txt="$(mktemp "${TMP_ROOT}/text.XXXXXX")"
    TMP_FILES+=("$txt")
    wav="$(mktemp "${TMP_ROOT}/audio.XXXXXX")"
    TMP_FILES+=("$wav")

    printf '%s\n' "$text" > "$txt"

    echo "STACKCHAN_OPENJTALK_BEGIN voice=${VOICE_PATH}"
    timeout --foreground --signal=TERM --kill-after=5s "${SYNTH_TIMEOUT}s" \
        open_jtalk \
        -x "$DICT_PATH" \
        -m "$VOICE_PATH" \
        -ow "$wav" \
        -r "$SPEED" \
        -fm "$PITCH_SHIFT" \
        -a "$ALPHA" \
        -g "$VOLUME_GAIN" \
        "$txt"

    set +e
    timeout --foreground --signal=TERM --kill-after=5s "${PLAYBACK_TIMEOUT}s" \
        aplay -q -D "$AUDIO_DEVICE" "$wav"
    play_status=$?
    set -e

    if [ "$play_status" -eq 124 ] || [ "$play_status" -ge 128 ]; then
        return "$play_status"
    fi
    if [ "$play_status" -ne 0 ]; then
        timeout --foreground --signal=TERM --kill-after=5s "${PLAYBACK_TIMEOUT}s" \
            aplay -q "$wav"
    fi

    echo "STACKCHAN_OPENJTALK_DONE"
}

main() {
    case "${1:-}" in
        --check)
            resolve_paths
            echo "STACKCHAN_OPENJTALK_READY dict=${DICT_PATH} voice=${VOICE_PATH} audio=${AUDIO_DEVICE}"
            ;;
        --text)
            shift
            speak_text "$*"
            ;;
        *)
            usage
            return 2
            ;;
    esac
}

main "$@"
