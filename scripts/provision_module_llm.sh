#!/usr/bin/env bash
# =============================================================================
# provision_module_llm.sh
# Host-side ADB wrapper for StackChan Module LLM setup.
#
# Usage:
#   1. Connect the Module LLM USB-C port to this PC.
#   2. Connect the Module LLM Ethernet port to the internet.
#   3. ./scripts/provision_module_llm.sh
#
# The actual setup logic lives in scripts/module_llm/ and runs on the Module LLM
# itself. This host-side wrapper transfers those scripts over ADB and invokes the
# selected setup target.
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MODULE_SCRIPT_DIR="${SCRIPT_DIR}/module_llm"
REMOTE_DIR_BASE="/tmp/stackchan_module_llm_setup"
REMOTE_DIR="${REMOTE_DIR_BASE}.$(date +%s).$$.${RANDOM}"
REMOTE_LOCK="/tmp/stackchan_module_llm_setup.lock"

ADB="${ADB:-adb}"
ADB_RETRY_COUNT=10
ADB_RETRY_DELAY=2
ADB_COMMAND_TIMEOUT="${ADB_COMMAND_TIMEOUT:-60}"
ADB_TRANSFER_TIMEOUT="${ADB_TRANSFER_TIMEOUT:-300}"
ADB_SETUP_TIMEOUT="${ADB_SETUP_TIMEOUT:-7200}"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

info() { echo -e "${CYAN}[INFO]${NC}  $*"; }
ok() { echo -e "${GREEN}[ OK ]${NC}  $*"; }
warn() { echo -e "${YELLOW}[WARN]${NC}  $*"; }
error() { echo -e "${RED}[ERR ]${NC}  $*"; }
die() { error "$*"; exit 1; }

usage() {
    cat <<'EOF'
Usage: ./scripts/provision_module_llm.sh [target] [options]

Targets:
  all        Run the full Module LLM setup. This is the default.
  repo       Register the StackFlow apt repository and run apt update.
  runtime    Install shared Module LLM runtime packages.
  whisper    Install Whisper ASR support and model.
  qwen3      Install Qwen3 local LLM support and model.
  vad        Install VAD support and model.
  melotts    Install MeloTTS fallback support and models.
  openjtalk  Install OpenJTalk, tohoku voice, and StackChan TTS helper.
  watchdog   Install automatic llm-sys hang recovery.
  verify     Verify installed packages and the OpenJTalk helper.

Options:
  --with-zh-tts    Install the optional Chinese MeloTTS model.
  --with-en-tts    Install the optional English MeloTTS models.
  --with-all-tts   Install all optional MeloTTS language models.
  --upgrade        Run apt upgrade during Module LLM setup.
  --reboot         Request a Linux reboot after setup. Some modules power off.
  --no-reboot      Do not reboot after setup. This is the default.
  -h, --help       Show this help.

Examples:
  ./scripts/provision_module_llm.sh
  ./scripts/provision_module_llm.sh qwen3 --no-reboot
  ./scripts/provision_module_llm.sh openjtalk --no-reboot
  ./scripts/provision_module_llm.sh melotts --with-en-tts

This script is the normal entry point from the host PC. It transfers the
module-side setup scripts over ADB and runs the selected target on the
Module LLM.

Environment:
  ADB_COMMAND_TIMEOUT   Timeout in seconds for short ADB operations (default: 60).
  ADB_TRANSFER_TIMEOUT  Timeout in seconds for the script transfer (default: 300).
  ADB_SETUP_TIMEOUT     Timeout in seconds for module setup (default: 7200).
EOF
}

TARGET="all"
RUN_UPGRADE=0
INSTALL_ZH_TTS=0
INSTALL_EN_TTS=0
REBOOT_MODE="no"

while [ "$#" -gt 0 ]; do
    case "$1" in
        --with-zh-tts)
            INSTALL_ZH_TTS=1
            ;;
        --with-en-tts)
            INSTALL_EN_TTS=1
            ;;
        --with-all-tts)
            INSTALL_ZH_TTS=1
            INSTALL_EN_TTS=1
            ;;
        --upgrade)
            RUN_UPGRADE=1
            ;;
        --reboot)
            REBOOT_MODE="yes"
            ;;
        --no-reboot)
            REBOOT_MODE="no"
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --*)
            usage
            die "Unknown option: $1"
            ;;
        *)
            if [ "$TARGET" != "all" ]; then
                usage
                die "Multiple targets were specified: $TARGET and $1"
            fi
            TARGET="$1"
            ;;
    esac
    shift
done

validate_timeout_setting() {
    local name="$1"
    local value="$2"

    [[ "$value" =~ ^[1-9][0-9]*$ ]] \
        || die "$name must be a positive integer number of seconds."
}

run_with_timeout() (
    local timeout_seconds="$1"
    shift

    local command_pid=""
    local watchdog_pid=""
    local timeout_command=""
    local watchdog_directory=""
    local timeout_marker=""

    cleanup_timeout_processes() {
        if [ -n "$watchdog_pid" ]; then
            kill "$watchdog_pid" 2>/dev/null || true
            wait "$watchdog_pid" 2>/dev/null || true
            watchdog_pid=""
        fi
        if [ -n "$command_pid" ] && kill -0 "$command_pid" 2>/dev/null; then
            kill -TERM "$command_pid" 2>/dev/null || true
            kill -KILL "$command_pid" 2>/dev/null || true
            wait "$command_pid" 2>/dev/null || true
            command_pid=""
        fi
        if [ -n "$watchdog_directory" ]; then
            rm -f -- "$timeout_marker"
            rmdir -- "$watchdog_directory" 2>/dev/null || true
            watchdog_directory=""
            timeout_marker=""
        fi
    }
    trap cleanup_timeout_processes EXIT
    trap 'exit 129' HUP
    trap 'exit 130' INT
    trap 'exit 143' TERM

    if command -v timeout >/dev/null 2>&1 &&
        timeout --help 2>&1 | grep -- '--kill-after' >/dev/null; then
        timeout_command="timeout"
    elif command -v gtimeout >/dev/null 2>&1 &&
        gtimeout --help 2>&1 | grep -- '--kill-after' >/dev/null; then
        timeout_command="gtimeout"
    fi

    if [ -n "$timeout_command" ]; then
        "$timeout_command" \
            --foreground \
            --signal=TERM \
            --kill-after=10s \
            "${timeout_seconds}s" \
            "$@"
        return
    fi

    local status

    watchdog_directory="$(mktemp -d "${TMPDIR:-/tmp}/stackchan_adb_timeout.XXXXXX")" \
        || {
            printf 'Failed to create timeout watchdog state directory.\n' >&2
            return 1
        }
    timeout_marker="${watchdog_directory}/expired"

    "$@" &
    command_pid=$!
    (
        local sleep_pid=""

        cleanup_watchdog() {
            if [ -n "$sleep_pid" ]; then
                kill "$sleep_pid" 2>/dev/null || true
                wait "$sleep_pid" 2>/dev/null || true
            fi
        }
        trap cleanup_watchdog EXIT
        trap 'exit 129' HUP
        trap 'exit 130' INT
        trap 'exit 143' TERM

        sleep "$timeout_seconds" &
        sleep_pid=$!
        if ! wait "$sleep_pid"; then
            exit 0
        fi
        sleep_pid=""

        if kill -0 "$command_pid" 2>/dev/null; then
            : > "$timeout_marker"
            printf 'ADB command timed out after %s seconds.\n' "$timeout_seconds" >&2
            kill -TERM "$command_pid" 2>/dev/null || true
            sleep 10 &
            sleep_pid=$!
            if ! wait "$sleep_pid"; then
                exit 0
            fi
            sleep_pid=""
            kill -KILL "$command_pid" 2>/dev/null || true
        fi
    ) &
    watchdog_pid=$!

    if wait "$command_pid"; then
        status=0
    else
        status=$?
    fi
    command_pid=""

    kill "$watchdog_pid" 2>/dev/null || true
    wait "$watchdog_pid" 2>/dev/null || true
    watchdog_pid=""
    if [ -e "$timeout_marker" ]; then
        status=124
    fi
    return "$status"
)

adb_shell_timeout() {
    local timeout_seconds="$1"
    shift

    local marker="__STACKCHAN_ADB_EXIT__"
    local output clean rc

    output="$(run_with_timeout "$timeout_seconds" "$ADB" shell "export DEBIAN_FRONTEND=noninteractive APT_LISTCHANGES_FRONTEND=none; $*; rc=\$?; printf '\n${marker}%s\n' \"\$rc\"" 2>&1)" \
        || {
            printf '%s\n' "$output"
            return 1
        }

    clean="$(printf '%s\n' "$output" | tr -d '\r')"
    rc="$(printf '%s\n' "$clean" | sed -n "s/^${marker}//p" | tail -n 1)"
    printf '%s\n' "$clean" | grep -v "^${marker}" || true

    [ "$rc" = "0" ]
}

adb_shell() {
    adb_shell_timeout "$ADB_COMMAND_TIMEOUT" "$@"
}

remote_quote_args() {
    local arg
    for arg in "$@"; do
        printf ' %q' "$arg"
    done
}

target_script() {
    case "$TARGET" in
        all|module|llm-module)
            echo "setup_llm_module.sh"
            ;;
        repo)
            echo "setup_repo.sh"
            ;;
        runtime)
            echo "setup_runtime.sh"
            ;;
        whisper)
            echo "setup_whisper.sh"
            ;;
        qwen3)
            echo "setup_qwen3.sh"
            ;;
        vad)
            echo "setup_vad.sh"
            ;;
        melotts)
            echo "setup_melotts.sh"
            ;;
        openjtalk)
            echo "setup_openjtalk.sh"
            ;;
        watchdog|llm-sys-watchdog)
            echo "setup_llm_sys_watchdog.sh"
            ;;
        verify)
            echo "verify_setup.sh"
            ;;
        *)
            usage
            die "Unknown target: $TARGET"
            ;;
    esac
}

build_target_args() {
    TARGET_ARGS=()

    case "$TARGET" in
        all|module|llm-module)
            TARGET_ARGS+=(--no-reboot)
            if [ "$RUN_UPGRADE" -eq 1 ]; then
                TARGET_ARGS+=(--upgrade)
            fi
            if [ "$INSTALL_ZH_TTS" -eq 1 ]; then
                TARGET_ARGS+=(--with-zh-tts)
            fi
            if [ "$INSTALL_EN_TTS" -eq 1 ]; then
                TARGET_ARGS+=(--with-en-tts)
            fi
            ;;
        repo)
            if [ "$RUN_UPGRADE" -eq 1 ]; then
                TARGET_ARGS+=(--upgrade)
            fi
            if [ "$INSTALL_ZH_TTS" -eq 1 ] || [ "$INSTALL_EN_TTS" -eq 1 ]; then
                die "TTS language model options are only valid for all, melotts, or verify targets."
            fi
            ;;
        melotts|verify)
            if [ "$INSTALL_ZH_TTS" -eq 1 ]; then
                TARGET_ARGS+=(--with-zh-tts)
            fi
            if [ "$INSTALL_EN_TTS" -eq 1 ]; then
                TARGET_ARGS+=(--with-en-tts)
            fi
            if [ "$RUN_UPGRADE" -eq 1 ]; then
                die "--upgrade is only valid for all or repo targets."
            fi
            ;;
        runtime|whisper|qwen3|vad|openjtalk|watchdog|llm-sys-watchdog)
            if [ "$RUN_UPGRADE" -eq 1 ]; then
                die "--upgrade is only valid for all or repo targets."
            fi
            if [ "$INSTALL_ZH_TTS" -eq 1 ] || [ "$INSTALL_EN_TTS" -eq 1 ]; then
                die "TTS language model options are only valid for all, melotts, or verify targets."
            fi
            ;;
    esac
}

check_adb() {
    command -v "$ADB" >/dev/null 2>&1 \
        || die "adb was not found. Install Android platform-tools and rerun this script."

    info "Checking ADB device..."
    run_with_timeout "$ADB_COMMAND_TIMEOUT" "$ADB" start-server >/dev/null 2>&1 || true

    if [ -n "${ANDROID_SERIAL:-}" ]; then
        local state
        state="$(run_with_timeout "$ADB_COMMAND_TIMEOUT" "$ADB" get-state 2>&1 || true)"
        if [ "$state" = "device" ]; then
            ok "Module LLM detected: $ANDROID_SERIAL"
            return
        fi

        error "ADB target $ANDROID_SERIAL is not ready."
        printf '%s\n' "$state"
        exit 1
    fi

    local devices=""
    local denied=""
    local unauthorized=""
    local count=0
    local attempt
    for attempt in $(seq 1 "$ADB_RETRY_COUNT"); do
        local adb_list
        adb_list="$(run_with_timeout "$ADB_COMMAND_TIMEOUT" "$ADB" devices |
            grep -v "List of" |
            grep -v '^$' || true)"
        devices="$(printf '%s\n' "$adb_list" | grep 'device$' || true)"
        denied="$(printf '%s\n' "$adb_list" | grep 'no permissions' || true)"
        unauthorized="$(printf '%s\n' "$adb_list" | grep 'unauthorized' || true)"
        count="$(printf '%s\n' "$devices" | grep -c 'device$' || true)"
        if [ "$count" -eq 1 ]; then
            ok "Module LLM detected"
            return
        elif [ "$count" -gt 1 ]; then
            error "Multiple ADB devices were detected. Disconnect other devices or set ANDROID_SERIAL."
            printf '%s\n' "$devices"
            exit 1
        elif [ -n "$denied" ]; then
            error "ADB can see the Module LLM, but this user has no USB permission."
            printf '%s\n' "$denied"
            echo ""
            echo "On Linux, start the ADB server with sufficient permission or add a udev rule."
            echo "Temporary workaround:"
            echo "  sudo adb kill-server && sudo adb start-server"
            exit 1
        elif [ -n "$unauthorized" ]; then
            error "ADB device is unauthorized."
            printf '%s\n' "$unauthorized"
            echo ""
            echo "Reconnect the Module LLM or confirm the authorization prompt if one appears."
            exit 1
        fi

        if [ "$attempt" -lt "$ADB_RETRY_COUNT" ]; then
            info "  Waiting for device (${attempt}/${ADB_RETRY_COUNT})..."
            sleep "$ADB_RETRY_DELAY"
        fi
    done

    echo ""
    error "Module LLM was not detected over ADB."
    echo ""
    echo "Checklist:"
    echo "  1. Is the cable connected to the Module LLM USB-C port, not the CoreS3 port?"
    echo "  2. Is the USB-C cable data-capable?"
    echo "  3. On Linux/macOS, try: sudo adb kill-server && sudo adb start-server"
    echo ""
    exit 1
}

push_module_scripts() {
    local required_files=(
        setup_common.sh
        setup_llm_module.sh
        setup_repo.sh
        setup_runtime.sh
        setup_whisper.sh
        setup_qwen3.sh
        setup_vad.sh
        setup_melotts.sh
        setup_openjtalk.sh
        setup_llm_sys_watchdog.sh
        verify_setup.sh
        openjtalk_tts.sh
        stackflow_pcm_ready.py
        stackchan_vad_pcm_bridge.py
        stackchan-vad-pcm-bridge.service
        stackchan_llm_sys_watchdog.py
        stackchan-llm-sys-watchdog.service
        llm-sys-stackchan-watchdog.conf
        stackchan_local_runtime.sh
    )
    local required_file
    for required_file in "${required_files[@]}"; do
        [ -f "${MODULE_SCRIPT_DIR}/${required_file}" ] \
            || die "Missing ${MODULE_SCRIPT_DIR}/${required_file}"
    done

    info "Transferring Module LLM setup scripts..."
    adb_shell "mkdir -m 0700 '$REMOTE_DIR'" >/dev/null \
        || die "Failed to prepare $REMOTE_DIR on the Module LLM."
    run_with_timeout "$ADB_TRANSFER_TIMEOUT" \
        "$ADB" push "${MODULE_SCRIPT_DIR}/." "${REMOTE_DIR}/" >/dev/null \
        || die "Failed to transfer Module LLM setup scripts."
    adb_shell "chmod 0755 '$REMOTE_DIR'/*.sh" >/dev/null \
        || die "Failed to chmod setup scripts on the Module LLM."
    ok "Setup scripts transferred"
}

run_module_setup() {
    local script
    local args
    local setup_command
    local quoted_setup_command

    script="$(target_script)"
    build_target_args
    args="$(remote_quote_args "${TARGET_ARGS[@]}")"
    setup_command="bash '$REMOTE_DIR/$script'${args}"
    printf -v quoted_setup_command '%q' "$setup_command"

    info "Running ${TARGET} setup on the Module LLM..."
    if ! adb_shell_timeout "$ADB_SETUP_TIMEOUT" \
        "flock -n '$REMOTE_LOCK' bash -c $quoted_setup_command"; then
        warn "The remote staging directory was retained for diagnosis: $REMOTE_DIR"
        die "Module LLM setup failed or another provisioning process holds $REMOTE_LOCK."
    fi
    ok "Module LLM ${TARGET} setup finished"

    adb_shell "rm -rf '$REMOTE_DIR'" >/dev/null \
        || warn "Failed to remove the remote staging directory: $REMOTE_DIR"
}

maybe_reboot() {
    echo ""

    case "$TARGET" in
        verify|watchdog|llm-sys-watchdog)
            if [ "$REBOOT_MODE" != "yes" ]; then
                ok "Reboot is not required for the ${TARGET} target."
                return
            fi
            ;;
    esac

    case "$REBOOT_MODE" in
        yes)
            info "Rebooting Module LLM..."
            if adb_shell "reboot"; then
                ok "Reboot command sent"
            else
                warn "ADB disconnected before reboot confirmation; verify that the Module LLM restarted."
            fi
            ;;
        no)
            ok "Reboot skipped. Control services are active; inference services wait for CoreS3."
            ;;
    esac
}

main() {
    echo ""
    echo "======================================================"
    echo " M5Stack Module LLM provisioning"
    echo "======================================================"
    echo ""

    validate_timeout_setting "ADB_COMMAND_TIMEOUT" "$ADB_COMMAND_TIMEOUT"
    validate_timeout_setting "ADB_TRANSFER_TIMEOUT" "$ADB_TRANSFER_TIMEOUT"
    validate_timeout_setting "ADB_SETUP_TIMEOUT" "$ADB_SETUP_TIMEOUT"
    check_adb
    adb_shell "command -v flock >/dev/null 2>&1" \
        || die "flock is required on the Module LLM for safe provisioning."
    push_module_scripts
    run_module_setup
    maybe_reboot

    echo ""
    echo "======================================================"
    echo " Setup complete"
    echo "======================================================"
    echo ""
    echo "The ${TARGET} target completed successfully."
    echo "Next steps for a full setup:"
    echo "  1. Disconnect the Module LLM USB cable and attach the module to the CoreS3."
    echo "  2. Flash the StackChan firmware."
    echo "  3. Power on the stack and open AI Agent from the face or its saved startup setting."
    echo "If a full power cycle is needed, use the hardware power switch."
    echo ""
}

main "$@"
