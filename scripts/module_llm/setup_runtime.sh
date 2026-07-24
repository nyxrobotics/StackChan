#!/usr/bin/env bash
# Install the Module LLM runtime packages shared by local conversation features.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=scripts/module_llm/setup_common.sh
source "${SCRIPT_DIR}/setup_common.sh"

usage() {
    cat <<'EOF'
Usage: setup_runtime.sh

Installs lib-llm, llm-sys, and llm-audio.
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
