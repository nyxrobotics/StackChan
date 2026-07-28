#!/usr/bin/env bash
# Run every StackChan Module LLM setup step in order.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=scripts/module_llm/setup_common.sh
source "${SCRIPT_DIR}/setup_common.sh"

RUN_UPGRADE=0
INSTALL_ZH_TTS=0
INSTALL_EN_TTS=0
REBOOT_MODE="no"

usage() {
    cat <<'EOF'
Usage: setup_llm_module.sh [options]

Options:
  --with-zh-tts    Install the optional Chinese MeloTTS model.
  --with-en-tts    Install the optional English MeloTTS models.
  --with-all-tts   Install all optional MeloTTS language models.
  --upgrade        Run apt upgrade after registering the apt repository.
  --reboot         Request a Linux reboot after setup. Some modules power off.
  --no-reboot      Do not reboot after setup. This is the default.
  -h, --help       Show this help.
EOF
}

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
        *)
            usage
            die "Unknown option: $1"
            ;;
    esac
    shift
done

maybe_reboot() {
    echo ""
    ok "Module LLM setup finished."

    case "$REBOOT_MODE" in
        yes)
            info "Rebooting Module LLM..."
            reboot
            ;;
        no)
            ok "Reboot skipped. Installed services are already active."
            ;;
    esac
}

main() {
    local repo_args=()
    local model_args=()

    if [ "$RUN_UPGRADE" -eq 1 ]; then
        repo_args+=(--upgrade)
    fi
    if [ "$INSTALL_ZH_TTS" -eq 1 ]; then
        model_args+=(--with-zh-tts)
    fi
    if [ "$INSTALL_EN_TTS" -eq 1 ]; then
        model_args+=(--with-en-tts)
    fi

    echo ""
    echo "======================================================"
    echo " StackChan Module LLM setup"
    echo "======================================================"
    echo ""

    require_root
    check_disk_space

    "${SCRIPT_DIR}/setup_repo.sh" "${repo_args[@]}"
    "${SCRIPT_DIR}/setup_runtime.sh"
    "${SCRIPT_DIR}/setup_whisper.sh"
    "${SCRIPT_DIR}/setup_qwen3.sh"
    "${SCRIPT_DIR}/setup_vad.sh"
    "${SCRIPT_DIR}/setup_melotts.sh" "${model_args[@]}"
    "${SCRIPT_DIR}/setup_openjtalk.sh"
    "${SCRIPT_DIR}/setup_llm_sys_watchdog.sh"
    "${SCRIPT_DIR}/verify_setup.sh" "${model_args[@]}"
    maybe_reboot
}

main "$@"
