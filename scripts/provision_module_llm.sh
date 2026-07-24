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
REMOTE_DIR="/tmp/stackchan_module_llm_setup"

ADB="${ADB:-adb}"
ADB_RETRY_COUNT=10
ADB_RETRY_DELAY=2

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
  verify     Verify installed packages and the OpenJTalk helper.

Options:
  --with-zh-tts    Install the optional Chinese MeloTTS model.
  --with-en-tts    Install the optional English MeloTTS models.
  --with-all-tts   Install all optional MeloTTS language models.
  --upgrade        Run apt upgrade during Module LLM setup.
  --reboot         Reboot automatically after setup.
  --no-reboot      Do not reboot after setup.
  -h, --help       Show this help.

Examples:
  ./scripts/provision_module_llm.sh
  ./scripts/provision_module_llm.sh qwen3 --no-reboot
  ./scripts/provision_module_llm.sh openjtalk --no-reboot
  ./scripts/provision_module_llm.sh melotts --with-en-tts

This script is the normal entry point from the host PC. It transfers the
module-side setup scripts over ADB and runs the selected target on the
Module LLM.
EOF
}

TARGET="all"
RUN_UPGRADE=0
INSTALL_ZH_TTS=0
INSTALL_EN_TTS=0
REBOOT_MODE="ask"

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

adb_shell() {
    local marker="__STACKCHAN_ADB_EXIT__"
    local output clean rc

    output="$("$ADB" shell "export DEBIAN_FRONTEND=noninteractive APT_LISTCHANGES_FRONTEND=none; $*; rc=\$?; printf '\n${marker}%s\n' \"\$rc\"" 2>&1)" \
        || {
            printf '%s\n' "$output"
            return 1
        }

    clean="$(printf '%s\n' "$output" | tr -d '\r')"
    rc="$(printf '%s\n' "$clean" | sed -n "s/^${marker}//p" | tail -n 1)"
    printf '%s\n' "$clean" | grep -v "^${marker}" || true

    [ "$rc" = "0" ]
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
        runtime|whisper|qwen3|vad|openjtalk)
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
    "$ADB" start-server >/dev/null 2>&1 || true

    if [ -n "${ANDROID_SERIAL:-}" ]; then
        local state
        state="$("$ADB" get-state 2>&1 || true)"
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
        adb_list="$("$ADB" devices | grep -v "List of" | grep -v '^$' || true)"
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
    [ -f "${MODULE_SCRIPT_DIR}/setup_llm_module.sh" ] \
        || die "Missing ${MODULE_SCRIPT_DIR}/setup_llm_module.sh"
    [ -f "${MODULE_SCRIPT_DIR}/openjtalk_tts.sh" ] \
        || die "Missing ${MODULE_SCRIPT_DIR}/openjtalk_tts.sh"

    info "Transferring Module LLM setup scripts..."
    adb_shell "rm -rf '$REMOTE_DIR' && mkdir -p '$REMOTE_DIR'" >/dev/null \
        || die "Failed to prepare $REMOTE_DIR on the Module LLM."
    "$ADB" push "${MODULE_SCRIPT_DIR}/." "${REMOTE_DIR}/" >/dev/null \
        || die "Failed to transfer Module LLM setup scripts."
    adb_shell "chmod 0755 '$REMOTE_DIR'/*.sh" >/dev/null \
        || die "Failed to chmod setup scripts on the Module LLM."
    ok "Setup scripts transferred"
}

run_module_setup() {
    local script
    local args
    script="$(target_script)"
    build_target_args
    args="$(remote_quote_args "${TARGET_ARGS[@]}")"

    info "Running ${TARGET} setup on the Module LLM..."
    adb_shell "bash '$REMOTE_DIR/$script'${args}" \
        || die "Module LLM setup failed."
    ok "Module LLM ${TARGET} setup finished"
}

maybe_reboot() {
    echo ""
    case "$REBOOT_MODE" in
        yes)
            info "Rebooting Module LLM..."
            adb_shell "reboot" || true
            ok "Reboot command sent"
            ;;
        no)
            warn "Reboot skipped. Run 'adb shell reboot' before starting StackChan."
            ;;
        ask)
            if [ -t 0 ]; then
                local answer
                read -r -p "Reboot Module LLM now? [Y/n]: " answer
                answer="${answer:-y}"
                case "${answer,,}" in
                    y|yes)
                        info "Rebooting Module LLM..."
                        adb_shell "reboot" || true
                        ok "Reboot command sent"
                        ;;
                    *)
                        warn "Reboot skipped. Run 'adb shell reboot' before starting StackChan."
                        ;;
                esac
            else
                warn "Non-interactive shell: reboot skipped. Run 'adb shell reboot' before starting StackChan."
            fi
            ;;
    esac
}

main() {
    echo ""
    echo "======================================================"
    echo " M5Stack Module LLM provisioning"
    echo "======================================================"
    echo ""

    check_adb
    push_module_scripts
    run_module_setup
    maybe_reboot

    echo ""
    echo "======================================================"
    echo " Setup complete"
    echo "======================================================"
    echo ""
    echo "Next steps:"
    echo "  1. After the Module LLM reboots, attach it to the CoreS3."
    echo "  2. Flash the StackChan firmware."
    echo "  3. Power on StackChan; the Module LLM pipeline will start automatically."
    echo ""
}

main "$@"
