#!/usr/bin/env bash
# Install OpenJTalk, the StackChan TTS helper, and the tohoku-f01 voice.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=scripts/module_llm/setup_common.sh
source "${SCRIPT_DIR}/setup_common.sh"

usage() {
    cat <<'EOF'
Usage: setup_openjtalk.sh

Installs OpenJTalk packages, places /opt/stackchan/openjtalk_tts.sh,
and downloads the tohoku-f01 neutral voice.

Environment:
  TOHOKU_VOICE_URL            Override the tohoku-f01 neutral voice URL.
  TOHOKU_COPYRIGHT_URL        Override the tohoku voice copyright URL.
  STACKCHAN_OPENJTALK_HELPER  Path to openjtalk_tts.sh to install.
  STACKCHAN_DOWNLOAD_TOTAL_TIMEOUT_SECONDS
                              Overall timeout for each download (default: 900).
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        -h|--help)
            usage
            exit 0
            ;;
        *)
            usage
            die "Unknown option: $1"
            ;;
    esac
done

install_openjtalk_helper() {
    info "Installing StackChan OpenJTalk helper..."
    mkdir -p /opt/stackchan/voices

    if [ -f "$OPENJTALK_HELPER_SOURCE" ]; then
        install_file_atomic "$OPENJTALK_HELPER_SOURCE" "$OPENJTALK_HELPER_TARGET" 0755 \
            || die "Failed to install $OPENJTALK_HELPER_TARGET."
        ok "$OPENJTALK_HELPER_TARGET"
    elif [ -x "$OPENJTALK_HELPER_TARGET" ]; then
        ok "$OPENJTALK_HELPER_TARGET (already installed)"
    else
        die "openjtalk_tts.sh was not found next to setup_openjtalk.sh. From the host PC, run: ./scripts/provision_module_llm.sh openjtalk"
    fi
}

validate_tohoku_voice() {
    local voice_file="$1"
    local size

    [ -s "$voice_file" ] || return 1
    size="$(wc -c < "$voice_file")"
    [ "$size" -ge 1048576 ] || return 1
    grep -q '^\[GLOBAL\]$' "$voice_file" &&
        grep -q '^\[STREAM\]$' "$voice_file" &&
        grep -q '^\[POSITION\]$' "$voice_file" &&
        grep -q '^\[DATA\]$' "$voice_file"
}

validate_tohoku_copyright() {
    local copyright_file="$1"

    [ -s "$copyright_file" ] &&
        grep -qi 'copyright' "$copyright_file"
}

install_tohoku_voice() {
    local voice_file="$TOHOKU_VOICE_TARGET"
    local copyright_file="$TOHOKU_COPYRIGHT_TARGET"

    info "Checking tohoku-f01 neutral voice..."
    mkdir -p /opt/stackchan/voices

    if validate_tohoku_voice "$voice_file"; then
        ok "tohoku-f01-neutral.htsvoice (already installed)"
    else
        if [ -e "$voice_file" ]; then
            warn "Existing tohoku-f01 neutral voice is invalid; downloading a replacement."
        fi
        info "Downloading tohoku-f01-neutral.htsvoice..."
        download_atomic "$TOHOKU_VOICE_URL" "$voice_file" validate_tohoku_voice \
            || die "Failed to download tohoku-f01-neutral.htsvoice."
        ok "tohoku-f01-neutral.htsvoice"
    fi

    if ! validate_tohoku_copyright "$copyright_file"; then
        if [ -e "$copyright_file" ]; then
            warn "Existing tohoku voice COPYRIGHT.txt is invalid; downloading a replacement."
        fi
        download_atomic "$TOHOKU_COPYRIGHT_URL" "$copyright_file" validate_tohoku_copyright \
            || warn "Failed to download tohoku voice COPYRIGHT.txt."
    fi
}

require_root
require_stackflow_repo
ensure_bootstrap_tools
install_package_group "OpenJTalk Japanese TTS" "${PACKAGES_OPENJTALK[@]}"
install_openjtalk_helper
install_tohoku_voice
"$OPENJTALK_HELPER_TARGET" --check || die "OpenJTalk helper check failed."
