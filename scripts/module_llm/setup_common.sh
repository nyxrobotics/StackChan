#!/usr/bin/env bash
# Shared helpers and package lists for Module LLM setup scripts.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

SOURCES_FILE="/etc/apt/sources.list.d/StackFlow.list"
STACKFLOW_KEY="/etc/apt/keyrings/StackFlow.gpg"
STACKFLOW_KEY_URL="https://repo.llm.m5stack.com/m5stack-apt-repo/key/StackFlow.gpg"
STACKFLOW_REPO="deb [arch=arm64 signed-by=/etc/apt/keyrings/StackFlow.gpg] https://repo.llm.m5stack.com/m5stack-apt-repo jammy ax630c"

TOHOKU_VOICE_URL="${TOHOKU_VOICE_URL:-https://raw.githubusercontent.com/icn-lab/htsvoice-tohoku-f01/master/tohoku-f01-neutral.htsvoice}"
TOHOKU_COPYRIGHT_URL="${TOHOKU_COPYRIGHT_URL:-https://raw.githubusercontent.com/icn-lab/htsvoice-tohoku-f01/master/COPYRIGHT.txt}"

OPENJTALK_HELPER_SOURCE="${STACKCHAN_OPENJTALK_HELPER:-${SCRIPT_DIR}/openjtalk_tts.sh}"
OPENJTALK_HELPER_TARGET="/opt/stackchan/openjtalk_tts.sh"

PACKAGES_RUNTIME=(
    lib-llm
    llm-sys
    llm-audio
)

PACKAGES_WHISPER=(
    llm-whisper
    llm-model-whisper-tiny
)

PACKAGES_QWEN3=(
    llm-llm
    llm-model-qwen3-0.6b-ax630c
)

PACKAGES_MELOTTS=(
    llm-melotts
    llm-model-melotts-ja-jp
)

PACKAGES_VAD=(
    llm-vad
    llm-model-silero-vad
)

PACKAGES_OPENJTALK=(
    open-jtalk
    open-jtalk-mecab-naist-jdic
    alsa-utils
)

PACKAGES_ZH_TTS=(
    llm-model-melotts-zh-cn
)

PACKAGES_EN_TTS=(
    llm-model-melotts-en-default
    llm-model-melotts-en-us
)

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

export DEBIAN_FRONTEND=noninteractive
export APT_LISTCHANGES_FRONTEND=none

require_root() {
    [ "$(id -u)" -eq 0 ] || die "Run this script as root on the Module LLM."
}

is_installed() {
    dpkg -s "$1" >/dev/null 2>&1
}

check_disk_space() {
    info "Checking disk space..."
    df -h / || true

    local avail_kb
    avail_kb="$(df / | awk 'NR == 2 {print $4}' | tr -d '\r\n' || echo 0)"
    if [[ "$avail_kb" =~ ^[0-9]+$ ]] && [ "$avail_kb" -lt 1048576 ]; then
        warn "Less than 1GB is available on /. Model installation may fail."
        if [ -t 0 ]; then
            local answer
            read -r -p "Continue anyway? [y/N]: " answer
            case "${answer,,}" in
                y|yes) ;;
                *) die "Aborted." ;;
            esac
        else
            warn "Continuing because this is a non-interactive shell."
        fi
    fi
}

ensure_bootstrap_tools() {
    local packages=()

    command -v wget >/dev/null 2>&1 || packages+=(wget)
    is_installed ca-certificates || packages+=(ca-certificates)

    if [ "${#packages[@]}" -eq 0 ]; then
        return
    fi

    info "Installing bootstrap tools: ${packages[*]}"
    apt-get update -qq
    apt-get install -y -qq "${packages[@]}"
}

require_stackflow_repo() {
    if [ ! -f "$SOURCES_FILE" ] || ! grep -q "repo.llm.m5stack.com/m5stack-apt-repo" "$SOURCES_FILE"; then
        die "StackFlow apt repository is not registered. Run setup_repo.sh first."
    fi
}

install_package() {
    local pkg="$1"
    if is_installed "$pkg"; then
        ok "$pkg (already installed)"
        return 0
    fi

    info "Installing $pkg..."
    if apt-get install -y -qq "$pkg"; then
        ok "$pkg"
    else
        error "$pkg installation failed"
        return 1
    fi
}

install_package_group() {
    local label="$1"
    shift

    echo ""
    info "=== ${label} ==="

    local failed=()
    local pkg
    for pkg in "$@"; do
        install_package "$pkg" || failed+=("$pkg")
    done

    if [ "${#failed[@]}" -gt 0 ]; then
        echo ""
        error "Failed packages in ${label}:"
        printf '  - %s\n' "${failed[@]}"
        return 1
    fi
}
