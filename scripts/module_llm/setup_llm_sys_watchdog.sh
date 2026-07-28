#!/usr/bin/env bash
# Install automatic recovery for llm-sys TCP and CoreS3 UART stalls.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=scripts/module_llm/setup_common.sh
source "${SCRIPT_DIR}/setup_common.sh"

usage() {
    cat <<'EOF'
Usage: setup_llm_sys_watchdog.sh

Installs and starts the StackChan llm-sys health watchdog.
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

require_root
command -v python3 >/dev/null 2>&1 || die "python3 is required for the llm-sys watchdog."
command -v systemctl >/dev/null 2>&1 || die "systemd is required for the llm-sys watchdog."

mkdir -p "$(dirname "$LLM_SYS_WATCHDOG_TARGET")"
mkdir -p "$(dirname "$LLM_SYS_WATCHDOG_DROPIN_TARGET")"
install_file_atomic "$LLM_SYS_WATCHDOG_SOURCE" "$LLM_SYS_WATCHDOG_TARGET" 0755 \
    || die "Failed to install $LLM_SYS_WATCHDOG_TARGET."
install_file_atomic "$LLM_SYS_WATCHDOG_SERVICE_SOURCE" "$LLM_SYS_WATCHDOG_SERVICE_TARGET" 0644 \
    || die "Failed to install $LLM_SYS_WATCHDOG_SERVICE_TARGET."
install_file_atomic "$LLM_SYS_WATCHDOG_DROPIN_SOURCE" "$LLM_SYS_WATCHDOG_DROPIN_TARGET" 0644 \
    || die "Failed to install $LLM_SYS_WATCHDOG_DROPIN_TARGET."

"$LLM_SYS_WATCHDOG_TARGET" --check || die "llm-sys watchdog check failed."
ensure_services_active "${SERVICES_LLM_SYS_WATCHDOG[@]}"
ok "llm-sys automatic recovery installed"
