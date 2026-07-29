#!/usr/bin/env bash
# Start and stop the Module LLM inference plane on instructions from CoreS3.

set -euo pipefail

SYSTEMCTL="${STACKCHAN_SYSTEMCTL:-systemctl}"
TIMEOUT="${STACKCHAN_TIMEOUT_COMMAND:-timeout}"
COMMAND_TIMEOUT_SECONDS="${STACKCHAN_RUNTIME_COMMAND_TIMEOUT_SECONDS:-30}"
ACTIVE_FILE="${STACKCHAN_LOCAL_RUNTIME_ACTIVE_FILE:-/run/stackchan-local-runtime.active}"
PAUSE_FILE="${STACKCHAN_VAD_PCM_BRIDGE_PAUSE_FILE:-/run/stackchan-vad-pcm-bridge.paused}"
LOCAL_TURN_FILE="${STACKCHAN_LOCAL_TURN_FILE:-/run/stackchan-local-turn.active}"

LOCAL_SERVICES=(
    llm-audio.service
    llm-vad.service
    llm-whisper.service
    llm-llm.service
    llm-melotts.service
    stackchan-vad-pcm-bridge.service
)

STOP_SERVICES=(
    stackchan-vad-pcm-bridge.service
    llm-melotts.service
    llm-llm.service
    llm-whisper.service
    llm-vad.service
    llm-audio.service
)

usage() {
    cat <<'EOF'
Usage: stackchan_local_runtime.sh start | stop | status | --check

The llm-sys control service remains active. The audio, VAD, Whisper, LLM,
TTS, and VAD bridge services run only while CoreS3 requests local inference.
EOF
}

validate_timeout() {
    [[ "$COMMAND_TIMEOUT_SECONDS" =~ ^[1-9][0-9]*$ ]] || {
        echo "STACKCHAN_LOCAL_RUNTIME_ERROR invalid command timeout" >&2
        return 1
    }
}

run_systemctl() {
    "$TIMEOUT" --foreground --signal=TERM --kill-after=5s \
        "${COMMAND_TIMEOUT_SECONDS}s" "$SYSTEMCTL" "$@"
}

service_is_active() {
    "$SYSTEMCTL" is-active --quiet "$1"
}

runtime_is_fully_active() {
    [ -f "$ACTIVE_FILE" ] || return 1

    local service
    for service in "${LOCAL_SERVICES[@]}"; do
        service_is_active "$service" || return 1
    done
}

verify_units_exist() {
    local service
    for service in llm-sys.service "${LOCAL_SERVICES[@]}"; do
        "$SYSTEMCTL" cat "$service" >/dev/null 2>&1 || {
            echo "STACKCHAN_LOCAL_RUNTIME_ERROR missing service: $service" >&2
            return 1
        }
    done
}

mark_audio_paused() {
    mkdir -p "$(dirname "$PAUSE_FILE")" "$(dirname "$ACTIVE_FILE")"
    printf 'paused\n' > "$PAUSE_FILE"
    rm -f -- "$LOCAL_TURN_FILE"
}

stop_openjtalk() {
    if [ "${STACKCHAN_SKIP_PROCESS_KILL:-0}" = "1" ]; then
        return
    fi
    pkill -f 'aplay.*/tmp/stackchan-openjtalk/' >/dev/null 2>&1 || true
    pkill -f 'open_jtalk.*/tmp/stackchan-openjtalk/' >/dev/null 2>&1 || true
}

start_runtime() {
    verify_units_exist
    service_is_active llm-sys.service || {
        echo "STACKCHAN_LOCAL_RUNTIME_ERROR llm-sys is not active" >&2
        return 1
    }

    local already_running=0
    if runtime_is_fully_active; then
        already_running=1
    fi

    # Keep capture paused until CoreS3 finishes constructing the complete
    # VAD/Whisper/LLM/TTS pipeline.
    mark_audio_paused
    run_systemctl start "${LOCAL_SERVICES[@]}"

    local service
    for service in "${LOCAL_SERVICES[@]}"; do
        service_is_active "$service" || {
            echo "STACKCHAN_LOCAL_RUNTIME_ERROR failed to start: $service" >&2
            return 1
        }
    done

    printf 'running\n' > "$ACTIVE_FILE"
    if [ "$already_running" -eq 0 ]; then
        echo "STACKCHAN_LOCAL_RUNTIME_STARTED"
    fi
    echo "STACKCHAN_LOCAL_RUNTIME_RUNNING"
}

stop_runtime() {
    # Remove the desired-state marker first so the watchdog cannot revive the
    # inference plane while shutdown is in progress.
    rm -f -- "$ACTIVE_FILE"
    mark_audio_paused
    stop_openjtalk

    if ! run_systemctl stop "${STOP_SERVICES[@]}"; then
        echo "STACKCHAN_LOCAL_RUNTIME_ERROR service stop timed out" >&2
        return 1
    fi

    local service
    for service in "${LOCAL_SERVICES[@]}"; do
        if service_is_active "$service"; then
            echo "STACKCHAN_LOCAL_RUNTIME_ERROR still active: $service" >&2
            return 1
        fi
    done

    echo "STACKCHAN_LOCAL_RUNTIME_STOPPED"
}

runtime_status() {
    local active=0
    local service
    for service in "${LOCAL_SERVICES[@]}"; do
        if service_is_active "$service"; then
            active=$((active + 1))
        fi
    done

    if [ "$active" -eq "${#LOCAL_SERVICES[@]}" ] && [ -f "$ACTIVE_FILE" ]; then
        echo "STACKCHAN_LOCAL_RUNTIME_RUNNING"
        return 0
    fi
    if [ "$active" -eq 0 ] && [ ! -f "$ACTIVE_FILE" ]; then
        echo "STACKCHAN_LOCAL_RUNTIME_STOPPED"
        return 0
    fi

    echo "STACKCHAN_LOCAL_RUNTIME_PARTIAL active=${active}/${#LOCAL_SERVICES[@]}" >&2
    return 1
}

main() {
    validate_timeout
    command -v "$SYSTEMCTL" >/dev/null 2>&1 || {
        echo "STACKCHAN_LOCAL_RUNTIME_ERROR systemctl not found" >&2
        return 1
    }
    command -v "$TIMEOUT" >/dev/null 2>&1 || {
        echo "STACKCHAN_LOCAL_RUNTIME_ERROR timeout not found" >&2
        return 1
    }

    case "${1:-}" in
        start)
            start_runtime
            ;;
        stop)
            stop_runtime
            ;;
        status)
            runtime_status
            ;;
        --check)
            echo "STACKCHAN_LOCAL_RUNTIME_READY"
            ;;
        -h|--help)
            usage
            ;;
        *)
            usage
            return 2
            ;;
    esac
}

main "$@"
