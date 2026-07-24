#!/usr/bin/env bash
# Install Qwen3 LLM support for local conversation.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=scripts/module_llm/setup_common.sh
source "${SCRIPT_DIR}/setup_common.sh"

usage() {
    cat <<'EOF'
Usage: setup_qwen3.sh

Installs the LLM function module and the Qwen3-0.6B model.
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
install_package_group "Qwen3 local LLM" "${PACKAGES_QWEN3[@]}"
