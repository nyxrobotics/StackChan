#!/usr/bin/env bash
# Verify the Module LLM packages and StackChan OpenJTalk helper.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=scripts/module_llm/setup_common.sh
source "${SCRIPT_DIR}/setup_common.sh"

CHECK_ZH_TTS=0
CHECK_EN_TTS=0

usage() {
    cat <<'EOF'
Usage: verify_setup.sh [options]

Options:
  --with-zh-tts    Also verify the optional Chinese MeloTTS model.
  --with-en-tts    Also verify the optional English MeloTTS models.
  --with-all-tts   Verify all optional MeloTTS language models.
  -h, --help       Show this help.
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --with-zh-tts)
            CHECK_ZH_TTS=1
            ;;
        --with-en-tts)
            CHECK_EN_TTS=1
            ;;
        --with-all-tts)
            CHECK_ZH_TTS=1
            CHECK_EN_TTS=1
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
require_stackflow_repo

packages=(
    "${PACKAGES_RUNTIME[@]}"
    "${PACKAGES_WHISPER[@]}"
    "${PACKAGES_QWEN3[@]}"
    "${PACKAGES_VAD[@]}"
    "${PACKAGES_MELOTTS[@]}"
    "${PACKAGES_OPENJTALK[@]}"
)

if [ "$CHECK_ZH_TTS" -eq 1 ]; then
    packages+=("${PACKAGES_ZH_TTS[@]}")
fi

if [ "$CHECK_EN_TTS" -eq 1 ]; then
    packages+=("${PACKAGES_EN_TTS[@]}")
fi

echo ""
info "=== Verification ==="

missing=()
outdated=()
for pkg in "${packages[@]}"; do
    if is_installed "$pkg"; then
        if is_package_current "$pkg"; then
            ok "$pkg $(installed_version "$pkg")"
        else
            installed="$(installed_version "$pkg")"
            candidate="$(candidate_version "$pkg")"
            error "$pkg is outdated (${installed:-unknown} -> ${candidate:-unknown})"
            outdated+=("$pkg")
        fi
    else
        error "$pkg"
        missing+=("$pkg")
    fi
done

if [ "${#missing[@]}" -gt 0 ]; then
    echo ""
    error "Missing packages:"
    printf '  - %s\n' "${missing[@]}"
    exit 1
fi

if [ "${#outdated[@]}" -gt 0 ]; then
    echo ""
    error "Outdated packages:"
    printf '  - %s\n' "${outdated[@]}"
    echo ""
    error "Run setup_llm_module.sh or the relevant feature setup script to upgrade them."
    exit 1
fi

required_executables=(
    "$OPENJTALK_HELPER_TARGET"
    "$PCM_READY_HELPER_TARGET"
    "$VAD_PCM_BRIDGE_TARGET"
    "$LLM_SYS_WATCHDOG_TARGET"
    "$LOCAL_RUNTIME_TARGET"
)

for executable in "${required_executables[@]}"; do
    [ -x "$executable" ] || die "Required executable is missing: $executable"
    ok "$executable"
done

installed_files=(
    "$OPENJTALK_HELPER_SOURCE|$OPENJTALK_HELPER_TARGET"
    "$PCM_READY_HELPER_SOURCE|$PCM_READY_HELPER_TARGET"
    "$VAD_PCM_BRIDGE_SOURCE|$VAD_PCM_BRIDGE_TARGET"
    "$VAD_PCM_BRIDGE_SERVICE_SOURCE|$VAD_PCM_BRIDGE_SERVICE_TARGET"
    "$LLM_SYS_WATCHDOG_SOURCE|$LLM_SYS_WATCHDOG_TARGET"
    "$LLM_SYS_WATCHDOG_SERVICE_SOURCE|$LLM_SYS_WATCHDOG_SERVICE_TARGET"
    "$LLM_SYS_WATCHDOG_DROPIN_SOURCE|$LLM_SYS_WATCHDOG_DROPIN_TARGET"
    "$LOCAL_RUNTIME_SOURCE|$LOCAL_RUNTIME_TARGET"
)

for file_pair in "${installed_files[@]}"; do
    source_file="${file_pair%%|*}"
    installed_file="${file_pair#*|}"
    [ -f "$source_file" ] || die "Verification source is missing: $source_file"
    [ -f "$installed_file" ] || die "Installed file is missing: $installed_file"
    cmp -s "$source_file" "$installed_file" \
        || die "Installed file differs from the provisioning bundle: $installed_file"
    ok "$installed_file matches the provisioning bundle"
done

openjtalk_status="$("$OPENJTALK_HELPER_TARGET" --check)" \
    || die "OpenJTalk helper check failed."
printf '%s\n' "$openjtalk_status"
[[ "$openjtalk_status" == *"voice=${TOHOKU_VOICE_TARGET}"* ]] \
    || die "OpenJTalk is not using the tohoku-f01 neutral voice."
[ -s "$TOHOKU_COPYRIGHT_TARGET" ] \
    || die "Tohoku voice copyright file is missing: $TOHOKU_COPYRIGHT_TARGET"

"$PCM_READY_HELPER_TARGET" --check \
    || die "StackFlow PCM readiness helper check failed."
"$VAD_PCM_BRIDGE_TARGET" --check \
    || die "VAD PCM bridge check failed."
"$LLM_SYS_WATCHDOG_TARGET" --check \
    || die "llm-sys watchdog check failed."
"$LOCAL_RUNTIME_TARGET" --check \
    || die "Local runtime helper check failed."

verify_qwen3_tokenizer_compat
verify_silero_vad_mode_config

command -v systemctl >/dev/null 2>&1 \
    || die "systemd is required to verify Module LLM services."

control_services=(
    "${SERVICES_RUNTIME[@]}"
    "${SERVICES_LLM_SYS_WATCHDOG[@]}"
)
failed_services=()
for service in "${control_services[@]}"; do
    if systemctl is-enabled --quiet "$service" &&
        systemctl is-active --quiet "$service"; then
        ok "$service enabled and active"
    else
        error "$service is not both enabled and active"
        failed_services+=("$service")
    fi
done

for service in "${SERVICES_LOCAL_ON_DEMAND[@]}"; do
    if ! systemctl cat "$service" >/dev/null 2>&1; then
        error "$service is not installed"
        failed_services+=("$service")
    elif systemctl is-enabled --quiet "$service"; then
        error "$service must be disabled at boot"
        failed_services+=("$service")
    else
        ok "$service disabled at boot (S3-controlled)"
    fi
done

if [ "${#failed_services[@]}" -gt 0 ]; then
    echo ""
    error "Service verification failed:"
    printf '  - %s\n' "${failed_services[@]}"
    exit 1
fi

"$LOCAL_RUNTIME_TARGET" status \
    || die "Local runtime services are in a partial state."

ok "Module LLM environment verification completed"
