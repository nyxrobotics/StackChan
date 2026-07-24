#!/usr/bin/env bash
# Install MeloTTS support and optional MeloTTS language models.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=scripts/module_llm/setup_common.sh
source "${SCRIPT_DIR}/setup_common.sh"

INSTALL_ZH_TTS=0
INSTALL_EN_TTS=0

usage() {
    cat <<'EOF'
Usage: setup_melotts.sh [options]

Options:
  --with-zh-tts    Install the optional Chinese MeloTTS model.
  --with-en-tts    Install the optional English MeloTTS models.
  --with-all-tts   Install all optional MeloTTS language models.
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

require_root
require_stackflow_repo
install_package_group "MeloTTS fallback TTS" "${PACKAGES_MELOTTS[@]}"
ensure_services_active "${SERVICES_MELOTTS[@]}"

if [ "$INSTALL_ZH_TTS" -eq 1 ]; then
    install_package_group "Optional Chinese MeloTTS model" "${PACKAGES_ZH_TTS[@]}"
fi

if [ "$INSTALL_EN_TTS" -eq 1 ]; then
    install_package_group "Optional English MeloTTS models" "${PACKAGES_EN_TTS[@]}"
fi
