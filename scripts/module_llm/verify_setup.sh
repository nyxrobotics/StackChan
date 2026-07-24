#!/usr/bin/env bash
# Verify the Module LLM packages and StackChan OpenJTalk helper.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=scripts/module_llm/setup_common.sh
source "${SCRIPT_DIR}/setup_common.sh"

CHECK_ZH_TTS=0
CHECK_EN_TTS=0

usage() {
    cat <<'EOF'
Usage: verify_setup.sh [options]

Options:
  --with-zh-tts    Also verify the optional Chinese MeloTTS model.
  --with-en-tts    Also verify the optional English MeloTTS models.
  --with-all-tts   Verify all optional MeloTTS language models.
  -h, --help       Show this help.
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --with-zh-tts)
            CHECK_ZH_TTS=1
            ;;
        --with-en-tts)
            CHECK_EN_TTS=1
            ;;
        --with-all-tts)
            CHECK_ZH_TTS=1
            CHECK_EN_TTS=1
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

packages=(
    "${PACKAGES_RUNTIME[@]}"
    "${PACKAGES_WHISPER[@]}"
    "${PACKAGES_QWEN3[@]}"
    "${PACKAGES_VAD[@]}"
    "${PACKAGES_MELOTTS[@]}"
    "${PACKAGES_OPENJTALK[@]}"
)

if [ "$CHECK_ZH_TTS" -eq 1 ]; then
    packages+=("${PACKAGES_ZH_TTS[@]}")
fi

if [ "$CHECK_EN_TTS" -eq 1 ]; then
    packages+=("${PACKAGES_EN_TTS[@]}")
fi

echo ""
info "=== Verification ==="

missing=()
for pkg in "${packages[@]}"; do
    if is_installed "$pkg"; then
        ok "$pkg"
    else
        error "$pkg"
        missing+=("$pkg")
    fi
done

if [ "${#missing[@]}" -gt 0 ]; then
    echo ""
    error "Missing packages:"
    printf '  - %s\n' "${missing[@]}"
    exit 1
fi

"$OPENJTALK_HELPER_TARGET" --check || die "OpenJTalk helper check failed."

if ps aux | grep stackflow | grep -v grep >/dev/null 2>&1; then
    ok "StackFlow process is running"
else
    warn "StackFlow process was not found. It should start after reboot."
fi
