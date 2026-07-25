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
GAIN_DB="${STACKCHAN_OPENJTALK_GAIN_DB:-0}"
MUTE="${STACKCHAN_OPENJTALK_MUTE:-0}"

usage() {
    echo "Usage: $0 --check | --text TEXT"
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

    if [ "$MUTE" = "1" ]; then
        echo "STACKCHAN_OPENJTALK_BEGIN muted=1"
        echo "STACKCHAN_OPENJTALK_DONE"
        return 0
    fi

    mkdir -p "$TMP_ROOT"

    local stamp txt wav
    stamp="$(date +%s%N)"
    txt="${TMP_ROOT}/${stamp}.txt"
    wav="${TMP_ROOT}/${stamp}.wav"

    printf '%s\n' "$text" > "$txt"

    echo "STACKCHAN_OPENJTALK_BEGIN voice=${VOICE_PATH}"
    open_jtalk \
        -x "$DICT_PATH" \
        -m "$VOICE_PATH" \
        -ow "$wav" \
        -r "$SPEED" \
        -fm "$PITCH_SHIFT" \
        -a "$ALPHA" \
        -g "$GAIN_DB" \
        "$txt"

    if ! aplay -q -D "$AUDIO_DEVICE" "$wav"; then
        aplay -q "$wav"
    fi

    rm -f "$txt" "$wav"
    echo "STACKCHAN_OPENJTALK_DONE"
}

main() {
    case "${1:-}" in
        --check)
            resolve_paths
            echo "STACKCHAN_OPENJTALK_READY dict=${DICT_PATH} voice=${VOICE_PATH} audio=${AUDIO_DEVICE} features=gain_db,mute"
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
