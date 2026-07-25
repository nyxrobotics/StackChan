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

validate_stackflow_key() {
    local key_file="$1"
    local first_byte
    local size

    [ -s "$key_file" ] || return 1
    size="$(wc -c < "$key_file")"
    [ "$size" -ge 512 ] && [ "$size" -le 1048576 ] || return 1

    first_byte="$(od -An -tx1 -N1 "$key_file" | tr -d '[:space:]')"
    case "$first_byte" in
        98|99|9a|9b|c6)
            return 0
            ;;
        *)
            return 1
            ;;
    esac
}

info "Checking M5Stack StackFlow apt repository..."
mkdir -p /etc/apt/keyrings /etc/apt/sources.list.d

if validate_stackflow_key "$STACKFLOW_KEY"; then
    ok "StackFlow apt key is already installed"
else
    if [ -e "$STACKFLOW_KEY" ]; then
        warn "Existing StackFlow apt key is invalid; downloading a replacement."
    fi
    info "Downloading StackFlow apt key..."
    download_atomic "$STACKFLOW_KEY_URL" "$STACKFLOW_KEY" validate_stackflow_key \
        || die "Failed to download the StackFlow apt key."
    ok "StackFlow apt key installed"
fi

if [ -f "$SOURCES_FILE" ] && grep -Fxq -- "$STACKFLOW_REPO" "$SOURCES_FILE"; then
    ok "StackFlow apt repository is already registered"
else
    info "Writing StackFlow apt source..."
    write_text_atomic "$SOURCES_FILE" "$STACKFLOW_REPO" 0644 \
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
