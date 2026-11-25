#!/usr/bin/env bash
set -u

# ============================================================================
# DF-OpenVCable macOS Self-Test (Read-Only Diagnostics)
# Mirrors tests/mcvad_selftest.ps1 on Windows.
# Exit code = number of failed checks (0 = all pass).
# ============================================================================

PASS_COUNT=0
FAIL_COUNT=0
WARN_COUNT=0

pass() { echo "[PASS] $1"; PASS_COUNT=$((PASS_COUNT + 1)); }
fail() { echo "[FAIL] $1"; FAIL_COUNT=$((FAIL_COUNT + 1)); }
warn() { echo "[WARN] $1"; WARN_COUNT=$((WARN_COUNT + 1)); }

DRIVER_PATH="/Library/Audio/Plug-Ins/HAL/DF-OpenVCable.driver"
BINARY_PATH="${DRIVER_PATH}/Contents/MacOS/DF-OpenVCable"
PLIST_PATH="${DRIVER_PATH}/Contents/Info.plist"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MACOS_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

echo "=== DF-OpenVCable macOS Self-Test ==="
echo "macOS: $(sw_vers -productVersion) ($(uname -m))"
echo ""

# 1. HAL Driver bundle exists
if [[ -d "${DRIVER_PATH}" && -f "${BINARY_PATH}" && -f "${PLIST_PATH}" ]]; then
    pass "Driver bundle installed at ${DRIVER_PATH}"
else
    fail "Driver bundle missing at ${DRIVER_PATH} (run macos/scripts/install.sh)"
fi

# 2. Info.plist valid
if [[ -f "${PLIST_PATH}" ]] && plutil -lint "${PLIST_PATH}" >/dev/null 2>&1; then
    pass "Driver Info.plist syntax valid"
else
    fail "Driver Info.plist missing or invalid"
fi

# 3. Code signature verification
if [[ -d "${DRIVER_PATH}" ]] && codesign --verify --deep --strict "${DRIVER_PATH}" >/dev/null 2>&1; then
    pass "Driver code signature valid"
else
    fail "Driver code signature invalid or unsigned (required on Apple Silicon)"
fi

# 4. Architecture match
HOST_ARCH="$(uname -m)"
if [[ -f "${BINARY_PATH}" ]]; then
    ARCHS="$(lipo -archs "${BINARY_PATH}" 2>/dev/null || true)"
    if [[ "${ARCHS}" == *"${HOST_ARCH}"* ]]; then
        pass "Driver Mach-O supports host architecture ${HOST_ARCH} (archs: ${ARCHS})"
    else
        fail "Driver Mach-O does not match host ${HOST_ARCH} (found: ${ARCHS})"
    fi
else
    fail "Cannot check Mach-O architecture (binary missing)"
fi

# 5. Factory symbol exported
if [[ -f "${BINARY_PATH}" ]] && nm -gU "${BINARY_PATH}" 2>/dev/null | grep -q "_DFOpenVCable_Create"; then
    pass "CFPlugIn factory symbol _DFOpenVCable_Create exported"
else
    fail "Factory symbol _DFOpenVCable_Create not exported"
fi

# 6. coreaudiod process running
if pgrep -x coreaudiod >/dev/null 2>&1; then
    pass "coreaudiod daemon is running (PID $(pgrep -x coreaudiod | head -n 1))"
else
    fail "coreaudiod daemon is not running"
fi

# 7. CoreAudio device enumeration (5 cables expected)
CTL=""
if command -v dfovc-ctl >/dev/null 2>&1; then
    CTL="dfovc-ctl"
elif [[ -x "${MACOS_ROOT}/build/dfovc-ctl" ]]; then
    CTL="${MACOS_ROOT}/build/dfovc-ctl"
elif [[ -x "${MACOS_ROOT}/dfovc-ctl" ]]; then
    CTL="${MACOS_ROOT}/dfovc-ctl"
fi

if [[ -n "${CTL}" ]]; then
    DETECTED="$("${CTL}" list 2>/dev/null | grep -c "\[DF-OpenVCable\]" || true)"
    if [[ "${DETECTED}" -eq 5 ]]; then
        pass "All 5 DF-OpenVCable virtual devices active in CoreAudio"
    else
        fail "Expected 5 DF-OpenVCable devices in CoreAudio, found ${DETECTED}"
    fi

    if "${CTL}" status >/dev/null 2>&1; then
        pass "Custom property IPC ('dfst') healthy"
    else
        fail "Custom property IPC failed (driver not loaded or stale coreaudiod)"
    fi
else
    warn "dfovc-ctl not found; falling back to system_profiler"
    SP_COUNT="$(system_profiler SPAudioDataType 2>/dev/null | grep -c "DF-OpenVCable " || true)"
    if [[ "${SP_COUNT}" -ge 5 ]]; then
        pass "system_profiler reports ${SP_COUNT} DF-OpenVCable entries"
    else
        fail "system_profiler reports only ${SP_COUNT} DF-OpenVCable entries (expected 5)"
    fi
fi

# 8. Control Panel App check
if [[ -d "/Applications/DF-OpenVCable Control Panel.app" ]]; then
    pass "Control Panel installed in /Applications"
else
    warn "Control Panel not installed in /Applications (optional)"
fi

echo ""
echo "Summary: ${PASS_COUNT} PASS, ${WARN_COUNT} WARN, ${FAIL_COUNT} FAIL"
exit "${FAIL_COUNT}"
