#!/usr/bin/env bash
set -euo pipefail

# ============================================================================
# DF-OpenVCable macOS Reload Script
# ============================================================================

echo "==> Restarting CoreAudio daemon (coreaudiod)..."
sudo launchctl kickstart -kp system/com.apple.audio.coreaudiod 2>/dev/null || sudo killall -9 coreaudiod 2>/dev/null || true
sleep 2

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MACOS_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

if command -v dfovc-ctl >/dev/null 2>&1; then
    dfovc-ctl list
    echo ""
    dfovc-ctl status
elif [[ -x "${MACOS_ROOT}/build/dfovc-ctl" ]]; then
    "${MACOS_ROOT}/build/dfovc-ctl" list
    echo ""
    "${MACOS_ROOT}/build/dfovc-ctl" status
else
    system_profiler SPAudioDataType | grep -A 6 "DF-OpenVCable" || echo "No DF-OpenVCable devices found."
fi
