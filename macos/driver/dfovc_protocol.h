#pragma once

#include <stdint.h>

// ============================================================================
// DF-OpenVCable macOS Driver Shared Protocol
// ----------------------------------------------------------------------------
// Shared identifiers and CoreAudio custom property selectors used between
// DF-OpenVCable.driver (AudioServerPlugIn), DF-OpenVCable Control Panel.app,
// and dfovc-ctl CLI.
// ============================================================================

#define DFOVC_BUNDLE_ID             "com.dfopenvcable.driver"
#define DFOVC_MANUFACTURER          "DF-OpenVCable"
#define DFOVC_MODEL_UID             "com.dfopenvcable.model"
#define DFOVC_DRIVER_VERSION        "1.0.0"

// Unique CFPlugIn Factory UUID (generated specifically for DF-OpenVCable macOS)
// Must match macos/driver/Info.plist exactly.
#define DFOVC_FACTORY_UUID_STR      "8E6B4F2A-3C19-4D87-A510-7F92D1E4C601"

// Number of independent virtual audio cables exposed by the single driver
#define DFOVC_MAX_CABLES            5
#define DFOVC_CHANNELS              2
#define DFOVC_BITS_PER_SAMPLE       32
#define DFOVC_BYTES_PER_SAMPLE      4
#define DFOVC_BYTES_PER_FRAME       (DFOVC_CHANNELS * DFOVC_BYTES_PER_SAMPLE)

#define DFOVC_DEFAULT_SAMPLE_RATE   48000.0
#define DFOVC_DEFAULT_BUFFER_MS     500
#define DFOVC_MIN_BUFFER_MS         50
#define DFOVC_MAX_BUFFER_MS         2000

// ZeroTimeStamp period in sample frames (Apple minimum is 10923)
#define DFOVC_ZERO_TIMESTAMP_PERIOD 16384
#define DFOVC_SAFETY_OFFSET_FRAMES  64

// Supported sample rates (matching Windows DF-OpenVCable + 88.2 kHz)
static const double kDFOVCSupportedSampleRates[] = {
    44100.0,
    48000.0,
    88200.0,
    96000.0,
    192000.0
};
#define DFOVC_NUM_SAMPLE_RATES (sizeof(kDFOVCSupportedSampleRates) / sizeof(kDFOVCSupportedSampleRates[0]))

// Custom CoreAudio Property Selectors (FourCC, marshaled as CFPropertyList)
//   'dfst' (PlugIn):  Read-only  CFDictionary -> full driver & 5-cable telemetry
//   'dfcs' (Device):  Read-only  CFDictionary -> single cable telemetry
//   'dfcc' (PlugIn/Device): Read/Write CFDictionary -> update cable configuration
//   'dfrs' (PlugIn/Device): Read/Write CFDictionary -> reset overrun/underrun counters
#define kDFOVCPropertyAllStatus     ((uint32_t)'dfst')
#define kDFOVCPropertyCableStatus   ((uint32_t)'dfcs')
#define kDFOVCPropertyCableConfig   ((uint32_t)'dfcc')
#define kDFOVCPropertyResetStats    ((uint32_t)'dfrs')

// Persistent storage key inside coreaudiod's sandboxed storage
#define DFOVC_STORAGE_KEY           "DFOpenVCableConfig_v1"

// Dictionary keys for CFPropertyList payloads
#define DFOVC_KEY_VERSION           "version"
#define DFOVC_KEY_CABLE_COUNT       "cableCount"
#define DFOVC_KEY_CABLES            "cables"

#define DFOVC_KEY_CABLE_INDEX       "cableIndex"
#define DFOVC_KEY_NAME              "name"
#define DFOVC_KEY_UID               "uid"
#define DFOVC_KEY_SAMPLE_RATE       "sampleRate"
#define DFOVC_KEY_BITS_PER_SAMPLE   "bitsPerSample"
#define DFOVC_KEY_CHANNELS          "channels"
#define DFOVC_KEY_BUFFER_MS         "bufferDurationMs"
#define DFOVC_KEY_BUFFER_BYTES      "bufferSizeBytes"
#define DFOVC_KEY_BUFFER_FILL       "currentBufferFill"
#define DFOVC_KEY_OVERRUNS          "overrunCount"
#define DFOVC_KEY_UNDERRUNS         "underrunCount"
#define DFOVC_KEY_RENDER_ACTIVE     "isRenderActive"
#define DFOVC_KEY_CAPTURE_ACTIVE    "isCaptureActive"
#define DFOVC_KEY_ENABLED           "isEnabled"
#define DFOVC_KEY_OUTPUT_VOLUME     "outputVolume"
#define DFOVC_KEY_INPUT_VOLUME      "inputVolume"
#define DFOVC_KEY_OUTPUT_MUTE       "outputMute"
#define DFOVC_KEY_INPUT_MUTE        "inputMute"
#define DFOVC_KEY_OUTPUT_PEAK       "outputPeak"
#define DFOVC_KEY_INPUT_PEAK        "inputPeak"
#define DFOVC_KEY_IO_CLIENTS        "ioClientCount"
