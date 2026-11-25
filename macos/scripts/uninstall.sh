#!/usr/bin/env bash
set -euo pipefail

# ============================================================================
# DF-OpenVCable macOS Uninstaller
# ============================================================================

echo "==> Removing DF-OpenVCable macOS components..."

if [[ -d "/Library/Audio/Plug-Ins/HAL/DF-OpenVCable.driver" ]]; then
    sudo rm -rf "/Library/Audio/Plug-Ins/HAL/DF-OpenVCable.driver"
    echo "    Removed /Library/Audio/Plug-Ins/HAL/DF-OpenVCable.driver"
else
    echo "    Driver not installed in /Library/Audio/Plug-Ins/HAL."
fi

if [[ -d "/Applications/DF-OpenVCable Control Panel.app" ]]; then
    sudo rm -rf "/Applications/DF-OpenVCable Control Panel.app"
    echo "    Removed /Applications/DF-OpenVCable Control Panel.app"
fi

if [[ -f "/usr/local/bin/dfovc-ctl" ]]; then
    sudo rm -f "/usr/local/bin/dfovc-ctl"
    echo "    Removed /usr/local/bin/dfovc-ctl"
fi

echo "==> Restarting CoreAudio daemon (coreaudiod)..."
sudo launchctl kickstart -kp system/com.apple.audio.coreaudiod 2>/dev/null || sudo killall -9 coreaudiod 2>/dev/null || true
sleep 2

echo "Uninstall complete."
