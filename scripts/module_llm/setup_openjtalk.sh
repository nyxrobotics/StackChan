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
        install -m 0755 "$OPENJTALK_HELPER_SOURCE" "$OPENJTALK_HELPER_TARGET" \
            || die "Failed to install $OPENJTALK_HELPER_TARGET."
        ok "$OPENJTALK_HELPER_TARGET"
    elif [ -x "$OPENJTALK_HELPER_TARGET" ]; then
        ok "$OPENJTALK_HELPER_TARGET (already installed)"
    else
        die "openjtalk_tts.sh was not found next to setup_openjtalk.sh. From the host PC, run: ./scripts/provision_module_llm.sh openjtalk"
    fi
}

install_tohoku_voice() {
    info "Checking tohoku-f01 neutral voice..."
    mkdir -p /opt/stackchan/voices

    if [ -f /opt/stackchan/voices/tohoku-f01-neutral.htsvoice ]; then
        ok "tohoku-f01-neutral.htsvoice (already installed)"
    else
        info "Downloading tohoku-f01-neutral.htsvoice..."
        wget -qO /opt/stackchan/voices/tohoku-f01-neutral.htsvoice "$TOHOKU_VOICE_URL" \
            || die "Failed to download tohoku-f01-neutral.htsvoice."
        ok "tohoku-f01-neutral.htsvoice"
    fi

    if [ ! -f /opt/stackchan/voices/tohoku-f01-COPYRIGHT.txt ]; then
        wget -qO /opt/stackchan/voices/tohoku-f01-COPYRIGHT.txt "$TOHOKU_COPYRIGHT_URL" \
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
