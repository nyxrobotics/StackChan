#!/usr/bin/env bash
# Install the Module LLM runtime packages shared by local conversation features.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=scripts/module_llm/setup_common.sh
source "${SCRIPT_DIR}/setup_common.sh"

usage() {
    cat <<'EOF'
Usage: setup_runtime.sh

Installs lib-llm, llm-sys, llm-audio, and the S3-controlled runtime helper.
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
require_stackflow_repo
install_package_group "Module LLM runtime" "${PACKAGES_RUNTIME[@]}"
mkdir -p "$(dirname "$PCM_READY_HELPER_TARGET")"
install_file_atomic "$PCM_READY_HELPER_SOURCE" "$PCM_READY_HELPER_TARGET" 0755 \
    || die "Failed to install $PCM_READY_HELPER_TARGET."
"$PCM_READY_HELPER_TARGET" --check || die "StackFlow PCM readiness helper check failed."
ok "$PCM_READY_HELPER_TARGET"
install_file_atomic "$LOCAL_RUNTIME_SOURCE" "$LOCAL_RUNTIME_TARGET" 0755 \
    || die "Failed to install $LOCAL_RUNTIME_TARGET."
"$LOCAL_RUNTIME_TARGET" --check || die "Local runtime helper check failed."
ok "$LOCAL_RUNTIME_TARGET"
ensure_services_active "${SERVICES_RUNTIME[@]}"
ensure_services_on_demand "${SERVICES_AUDIO[@]}"
