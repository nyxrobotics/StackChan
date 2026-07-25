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
QWEN3_MODEL_ID="qwen3-0.6B-ax630c"
QWEN3_TOKENIZER_SCRIPT="/opt/m5stack/scripts/tokenizer_${QWEN3_MODEL_ID}.py"
QWEN3_TOKENIZER_COMPAT="/opt/m5stack/scripts/${QWEN3_MODEL_ID}_tokenizer.py"

DOWNLOAD_TIMEOUT_SECONDS="${STACKCHAN_DOWNLOAD_TIMEOUT_SECONDS:-30}"
DOWNLOAD_RETRIES="${STACKCHAN_DOWNLOAD_RETRIES:-3}"
DOWNLOAD_TOTAL_TIMEOUT_SECONDS="${STACKCHAN_DOWNLOAD_TOTAL_TIMEOUT_SECONDS:-900}"

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

SERVICES_RUNTIME=(
    llm-sys.service
    llm-audio.service
)

SERVICES_WHISPER=(
    llm-whisper.service
)

SERVICES_QWEN3=(
    llm-llm.service
)

SERVICES_MELOTTS=(
    llm-melotts.service
)

SERVICES_VAD=(
    llm-vad.service
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

installed_version() {
    dpkg-query -W -f='${Version}' "$1" 2>/dev/null || true
}

candidate_version() {
    apt-cache policy "$1" 2>/dev/null | awk '/Candidate:/ {print $2; exit}'
}

is_package_current() {
    local pkg="$1"
    local installed candidate
    installed="$(installed_version "$pkg")"
    candidate="$(candidate_version "$pkg")"

    [ -n "$installed" ] &&
        [ -n "$candidate" ] &&
        [ "$candidate" != "(none)" ] &&
        [ "$installed" = "$candidate" ]
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
    if ! command -v timeout >/dev/null 2>&1 ||
        ! timeout --help 2>&1 | grep -- '--kill-after' >/dev/null; then
        packages+=(coreutils)
    fi

    if [ "${#packages[@]}" -gt 0 ]; then
        info "Installing bootstrap tools: ${packages[*]}"
        apt-get update -qq
        apt-get install -y -qq "${packages[@]}"
    fi

    command -v timeout >/dev/null 2>&1 &&
        timeout --help 2>&1 | grep -- '--kill-after' >/dev/null \
        || die "GNU timeout is required for bounded setup operations."
}

download_atomic() (
    local url="$1"
    local destination="$2"
    local validator="$3"
    local mode="${4:-0644}"
    local temporary=""

    [[ "$DOWNLOAD_TIMEOUT_SECONDS" =~ ^[1-9][0-9]*$ ]] \
        || die "STACKCHAN_DOWNLOAD_TIMEOUT_SECONDS must be a positive integer."
    [[ "$DOWNLOAD_RETRIES" =~ ^[1-9][0-9]*$ ]] \
        || die "STACKCHAN_DOWNLOAD_RETRIES must be a positive integer."
    [[ "$DOWNLOAD_TOTAL_TIMEOUT_SECONDS" =~ ^[1-9][0-9]*$ ]] \
        || die "STACKCHAN_DOWNLOAD_TOTAL_TIMEOUT_SECONDS must be a positive integer."

    temporary="$(mktemp "${destination}.tmp.XXXXXX")" \
        || die "Failed to create a temporary file next to $destination."

    cleanup_download() {
        if [ -n "$temporary" ]; then
            rm -f -- "$temporary"
        fi
    }
    trap cleanup_download EXIT
    trap 'exit 129' HUP
    trap 'exit 130' INT
    trap 'exit 143' TERM

    timeout --foreground --signal=TERM --kill-after=10s \
        "${DOWNLOAD_TOTAL_TIMEOUT_SECONDS}s" \
        wget --quiet \
            --timeout="$DOWNLOAD_TIMEOUT_SECONDS" \
            --tries="$DOWNLOAD_RETRIES" \
            --waitretry=2 \
            --retry-connrefused \
            -O "$temporary" \
            "$url" \
        || return 1

    "$validator" "$temporary" || return 1
    chmod "$mode" "$temporary" || return 1
    mv -f -- "$temporary" "$destination" || return 1
    temporary=""
)

install_file_atomic() (
    local source="$1"
    local destination="$2"
    local mode="${3:-0644}"
    local temporary=""

    [ -f "$source" ] || return 1
    temporary="$(mktemp "${destination}.tmp.XXXXXX")" \
        || die "Failed to create a temporary file next to $destination."

    cleanup_install() {
        if [ -n "$temporary" ]; then
            rm -f -- "$temporary"
        fi
    }
    trap cleanup_install EXIT
    trap 'exit 129' HUP
    trap 'exit 130' INT
    trap 'exit 143' TERM

    cp -- "$source" "$temporary" || return 1
    [ -s "$temporary" ] || return 1
    chmod "$mode" "$temporary" || return 1
    mv -f -- "$temporary" "$destination" || return 1
    temporary=""
)

write_text_atomic() (
    local destination="$1"
    local content="$2"
    local mode="${3:-0644}"
    local temporary=""

    temporary="$(mktemp "${destination}.tmp.XXXXXX")" \
        || die "Failed to create a temporary file next to $destination."

    cleanup_write() {
        if [ -n "$temporary" ]; then
            rm -f -- "$temporary"
        fi
    }
    trap cleanup_write EXIT
    trap 'exit 129' HUP
    trap 'exit 130' INT
    trap 'exit 143' TERM

    printf '%s\n' "$content" > "$temporary" || return 1
    chmod "$mode" "$temporary" || return 1
    mv -f -- "$temporary" "$destination" || return 1
    temporary=""
)

require_stackflow_repo() {
    if [ ! -f "$SOURCES_FILE" ] || ! grep -Fxq -- "$STACKFLOW_REPO" "$SOURCES_FILE"; then
        die "StackFlow apt repository is not registered. Run setup_repo.sh first."
    fi
}

install_package() {
    local pkg="$1"
    local installed candidate

    if is_installed "$pkg"; then
        installed="$(installed_version "$pkg")"
        candidate="$(candidate_version "$pkg")"
        if is_package_current "$pkg"; then
            ok "$pkg ${installed} (already current)"
            return 0
        fi

        if [ -n "$candidate" ] && [ "$candidate" != "(none)" ]; then
            info "Upgrading $pkg: ${installed} -> ${candidate}"
        else
            info "Refreshing installed package $pkg (${installed})"
        fi
    else
        info "Installing $pkg..."
    fi

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

ensure_services_active() {
    if ! command -v systemctl >/dev/null 2>&1; then
        warn "systemctl is not available; skipping service activation."
        return 0
    fi

    local services=("$@")
    if [ "${#services[@]}" -eq 0 ]; then
        return 0
    fi

    info "Enabling and starting services: ${services[*]}"
    if ! systemctl daemon-reload; then
        error "systemd daemon reload failed"
        return 1
    fi
    systemctl enable --now "${services[@]}"

    local service
    local failed=()
    for service in "${services[@]}"; do
        if systemctl is-active --quiet "$service"; then
            ok "$service active"
        else
            error "$service is not active"
            failed+=("$service")
        fi
    done

    if [ "${#failed[@]}" -gt 0 ]; then
        return 1
    fi
}

ensure_qwen3_tokenizer_compat() (
    local temporary_directory=""
    local temporary_link=""

    cleanup_tokenizer_link() {
        if [ -n "$temporary_link" ]; then
            rm -f -- "$temporary_link"
        fi
        if [ -n "$temporary_directory" ]; then
            rmdir -- "$temporary_directory" 2>/dev/null || true
        fi
    }
    trap cleanup_tokenizer_link EXIT
    trap 'exit 129' HUP
    trap 'exit 130' INT
    trap 'exit 143' TERM

    if [ ! -f "$QWEN3_TOKENIZER_SCRIPT" ]; then
        die "Qwen3 tokenizer script was not found: $QWEN3_TOKENIZER_SCRIPT"
    fi

    if [ -e "$QWEN3_TOKENIZER_COMPAT" ] && [ ! -L "$QWEN3_TOKENIZER_COMPAT" ]; then
        error "Qwen3 tokenizer compatibility path exists and is not a symlink: $QWEN3_TOKENIZER_COMPAT"
        return 1
    fi

    temporary_directory="$(mktemp -d "${QWEN3_TOKENIZER_COMPAT}.tmp.XXXXXX")" \
        || die "Failed to create a temporary tokenizer link directory."
    temporary_link="${temporary_directory}/link"
    ln -s "$(basename "$QWEN3_TOKENIZER_SCRIPT")" "$temporary_link"
    mv -Tf -- "$temporary_link" "$QWEN3_TOKENIZER_COMPAT"
    temporary_link=""
    rmdir -- "$temporary_directory"
    temporary_directory=""
    ok "Qwen3 tokenizer compatibility link: $QWEN3_TOKENIZER_COMPAT"
)
