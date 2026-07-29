#!/usr/bin/env bash
# Install VAD support for local conversation.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=scripts/module_llm/setup_common.sh
source "${SCRIPT_DIR}/setup_common.sh"

usage() {
    cat <<'EOF'
Usage: setup_vad.sh

Installs the VAD function module and the Silero VAD model.
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
install_package_group "Voice activity detection" "${PACKAGES_VAD[@]}"
ensure_silero_vad_mode_config
mkdir -p "$(dirname "$VAD_PCM_BRIDGE_TARGET")"
install_file_atomic "$VAD_PCM_BRIDGE_SOURCE" "$VAD_PCM_BRIDGE_TARGET" 0755 \
    || die "Failed to install $VAD_PCM_BRIDGE_TARGET."
install_file_atomic "$VAD_PCM_BRIDGE_SERVICE_SOURCE" "$VAD_PCM_BRIDGE_SERVICE_TARGET" 0644 \
    || die "Failed to install $VAD_PCM_BRIDGE_SERVICE_TARGET."
"$VAD_PCM_BRIDGE_TARGET" --check || die "VAD PCM bridge check failed."
ok "$VAD_PCM_BRIDGE_TARGET"
ensure_services_on_demand "${SERVICES_VAD[@]}"
