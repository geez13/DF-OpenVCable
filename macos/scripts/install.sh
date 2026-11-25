#!/usr/bin/env bash
set -euo pipefail

# ============================================================================
# DF-OpenVCable macOS Installer
# ----------------------------------------------------------------------------
# Installs:
#   - /Library/Audio/Plug-Ins/HAL/DF-OpenVCable.driver
#   - /Applications/DF-OpenVCable Control Panel.app  (if present)
#   - /usr/local/bin/dfovc-ctl                       (if present)
# Then restarts coreaudiod and verifies all 5 virtual cables are active.
# ============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MACOS_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# Support running either from source tree (macos/build/...) or extracted release zip
if [[ -d "${MACOS_ROOT}/build/DF-OpenVCable.driver" ]]; then
    DIST_DIR="${MACOS_ROOT}/build"
elif [[ -d "${MACOS_ROOT}/DF-OpenVCable.driver" ]]; then
    DIST_DIR="${MACOS_ROOT}"
else
    echo "DF-OpenVCable.driver not found. Building from source first..."
    make -C "${MACOS_ROOT}" all
    DIST_DIR="${MACOS_ROOT}/build"
fi

DRIVER_SRC="${DIST_DIR}/DF-OpenVCable.driver"
APP_SRC="${DIST_DIR}/DF-OpenVCable Control Panel.app"
CLI_SRC="${DIST_DIR}/dfovc-ctl"

HAL_DIR="/Library/Audio/Plug-Ins/HAL"
DRIVER_DST="${HAL_DIR}/DF-OpenVCable.driver"

echo "==> Installing DF-OpenVCable macOS AudioServerPlugIn..."
if [[ ! -d "${DRIVER_SRC}" ]]; then
    echo "Error: ${DRIVER_SRC} does not exist." >&2
    exit 1
fi

sudo mkdir -p "${HAL_DIR}"
sudo rm -rf "${DRIVER_DST}"
sudo cp -R "${DRIVER_SRC}" "${DRIVER_DST}"
sudo chown -R root:wheel "${DRIVER_DST}"
sudo chmod -R 755 "${DRIVER_DST}"
sudo xattr -dr com.apple.quarantine "${DRIVER_DST}" 2>/dev/null || true

if [[ -d "${APP_SRC}" ]]; then
    echo "==> Installing DF-OpenVCable Control Panel.app to /Applications..."
    sudo rm -rf "/Applications/DF-OpenVCable Control Panel.app"
    sudo cp -R "${APP_SRC}" "/Applications/DF-OpenVCable Control Panel.app"
    sudo xattr -dr com.apple.quarantine "/Applications/DF-OpenVCable Control Panel.app" 2>/dev/null || true
fi

if [[ -f "${CLI_SRC}" ]]; then
    echo "==> Installing dfovc-ctl to /usr/local/bin/dfovc-ctl..."
    sudo mkdir -p /usr/local/bin
    sudo cp "${CLI_SRC}" /usr/local/bin/dfovc-ctl
    sudo chmod 755 /usr/local/bin/dfovc-ctl
    sudo xattr -d com.apple.quarantine /usr/local/bin/dfovc-ctl 2>/dev/null || true
fi

echo "==> Restarting CoreAudio daemon (coreaudiod)..."
sudo launchctl kickstart -kp system/com.apple.audio.coreaudiod 2>/dev/null || sudo killall -9 coreaudiod 2>/dev/null || true

echo "==> Waiting for CoreAudio to reload..."
sleep 2

if command -v dfovc-ctl >/dev/null 2>&1; then
    dfovc-ctl status || true
elif [[ -x "${CLI_SRC}" ]]; then
    "${CLI_SRC}" status || true
fi

echo ""
echo "Installation complete! All 5 cables (DF-OpenVCable A .. E) are ready."
echo "Open 'DF-OpenVCable Control Panel' from /Applications or run 'dfovc-ctl status'."
