#!/usr/bin/env bash
# Register the M5Stack StackFlow apt repository and refresh package indexes.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=scripts/module_llm/setup_common.sh
source "${SCRIPT_DIR}/setup_common.sh"

RUN_UPGRADE=0

usage() {
    cat <<'EOF'
Usage: setup_repo.sh [--upgrade]

Registers the StackFlow apt repository and runs apt update.
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --upgrade)
            RUN_UPGRADE=1
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
ensure_bootstrap_tools

info "Checking M5Stack StackFlow apt repository..."
mkdir -p /etc/apt/keyrings /etc/apt/sources.list.d

if [ ! -f "$STACKFLOW_KEY" ]; then
    info "Downloading StackFlow apt key..."
    wget -qO "$STACKFLOW_KEY" "$STACKFLOW_KEY_URL" \
        || die "Failed to download the StackFlow apt key."
else
    ok "StackFlow apt key is already installed"
fi

if [ -f "$SOURCES_FILE" ] && grep -q "repo.llm.m5stack.com/m5stack-apt-repo" "$SOURCES_FILE"; then
    ok "StackFlow apt repository is already registered"
else
    info "Writing StackFlow apt source..."
    printf '%s\n' "$STACKFLOW_REPO" > "$SOURCES_FILE" \
        || die "Failed to write $SOURCES_FILE."
    ok "StackFlow apt repository registered"
fi

info "Running apt update..."
apt-get update -qq || die "apt update failed."
ok "apt update finished"

if [ "$RUN_UPGRADE" -eq 1 ]; then
    info "Running apt upgrade..."
    apt-get install -y -qq apt-utils || true
    apt-get upgrade -y -qq || die "apt upgrade failed."
    ok "apt upgrade finished"
fi
