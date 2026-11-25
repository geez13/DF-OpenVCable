// ============================================================================
// DF-OpenVCable macOS CoreAudio HAL AudioServerPlugIn
// ----------------------------------------------------------------------------
// All-in-One Single Driver exposing 5 independent duplex virtual audio cables:
//   1. DF-OpenVCable A  (Output + Input loopback)
//   2. DF-OpenVCable B  (Output + Input loopback)
//   3. DF-OpenVCable C  (Output + Input loopback)
//   4. DF-OpenVCable D  (Output + Input loopback)
//   5. DF-OpenVCable E  (Output + Input loopback)
//
// Features:
//   - Zero-clock-drift timing anchored to mach_absolute_time()
//   - Lock-free, realtime-safe timestamp-indexed loopback ring buffer per cable
//   - Dynamic per-cable sample rate (44.1 / 48 / 88.2 / 96 / 192 kHz)
//   - Dynamic per-cable buffer duration (50 ms .. 2000 ms)
//   - Native macOS Volume & Mute controls (Input & Output)
//   - Sandbox-safe IPC via CoreAudio Custom Properties ('dfst','dfcs','dfcc','dfrs')
//   - Persistent per-cable settings via AudioServerPlugInHostInterface storage
// ============================================================================

#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <mach/mach_time.h>
#include <pthread.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <new>

#include "dfovc_protocol.h"
#include "ring_buffer.h"

namespace {

// ============================================================================
// Constants & AudioObjectID Layout
// ============================================================================

constexpr AudioObjectID kObjectID_PlugIn = kAudioObjectPlugInObject; // 1
constexpr AudioObjectID kCableObjectBase = 100;
constexpr AudioObjectID kCableObjectStride = 20;

enum class ObjectKind : uint32_t {
    Device       = 0,
    StreamInput  = 1,
    StreamOutput = 2,
    VolumeOutput = 3,
    MuteOutput   = 4,
    VolumeInput  = 5,
    MuteInput    = 6,
    Invalid      = 99
};

inline AudioObjectID MakeObjectID(uint32_t cableIdx, ObjectKind kind) {
    return kCableObjectBase + cableIdx * kCableObjectStride + static_cast<uint32_t>(kind);
}

inline bool DecodeObjectID(AudioObjectID objID, uint32_t& outCableIdx, ObjectKind& outKind) {
    if (objID < kCableObjectBase) return false;
    const uint32_t offset = objID - kCableObjectBase;
    const uint32_t idx = offset / kCableObjectStride;
    const uint32_t sub = offset % kCableObjectStride;
    if (idx >= DFOVC_MAX_CABLES || sub > static_cast<uint32_t>(ObjectKind::MuteInput)) {
        return false;
    }
    outCableIdx = idx;
    outKind = static_cast<ObjectKind>(sub);
    return true;
}

constexpr float kVolumeMinDB = -96.0f;
constexpr float kVolumeMaxDB = 0.0f;

static const char* const kCableNames[DFOVC_MAX_CABLES] = {
    "DF-OpenVCable A",
    "DF-OpenVCable B",
    "DF-OpenVCable C",
    "DF-OpenVCable D",
    "DF-OpenVCable E"
};

static const char* const kCableUIDs[DFOVC_MAX_CABLES] = {
    "com.dfopenvcable.device.cable_a",
    "com.dfopenvcable.device.cable_b",
    "com.dfopenvcable.device.cable_c",
    "com.dfopenvcable.device.cable_d",
    "com.dfopenvcable.device.cable_e"
};

// Custom property descriptors
static const AudioServerPlugInCustomPropertyInfo kPlugInCustomProperties[] = {
    { kDFOVCPropertyAllStatus,   kAudioServerPlugInCustomPropertyDataTypeCFPropertyList, kAudioServerPlugInCustomPropertyDataTypeNone },
    { kDFOVCPropertyCableConfig, kAudioServerPlugInCustomPropertyDataTypeCFPropertyList, kAudioServerPlugInCustomPropertyDataTypeNone },
    { kDFOVCPropertyResetStats,  kAudioServerPlugInCustomPropertyDataTypeCFPropertyList, kAudioServerPlugInCustomPropertyDataTypeNone }
};

static const AudioServerPlugInCustomPropertyInfo kDeviceCustomProperties[] = {
    { kDFOVCPropertyCableStatus, kAudioServerPlugInCustomPropertyDataTypeCFPropertyList, kAudioServerPlugInCustomPropertyDataTypeNone },
    { kDFOVCPropertyCableConfig, kAudioServerPlugInCustomPropertyDataTypeCFPropertyList, kAudioServerPlugInCustomPropertyDataTypeNone },
    { kDFOVCPropertyResetStats,  kAudioServerPlugInCustomPropertyDataTypeCFPropertyList, kAudioServerPlugInCustomPropertyDataTypeNone }
};

// ============================================================================
// Helper Conversions
// ============================================================================

inline float ClampScalar(float s) {
    return std::max(0.0f, std::min(1.0f, s));
}

inline float ScalarToDecibels(float scalar) {
    scalar = ClampScalar(scalar);
    if (scalar <= 0.0f) return kVolumeMinDB;
    return kVolumeMinDB + scalar * (kVolumeMaxDB - kVolumeMinDB);
}

inline float DecibelsToScalar(float dB) {
    if (dB <= kVolumeMinDB) return 0.0f;
    if (dB >= kVolumeMaxDB) return 1.0f;
    return (dB - kVolumeMinDB) / (kVolumeMaxDB - kVolumeMinDB);
}

inline float ScalarToLinearGain(float scalar) {
    scalar = ClampScalar(scalar);
    if (scalar <= 0.0f) return 0.0f;
    if (scalar >= 1.0f) return 1.0f;
    const float dB = ScalarToDecibels(scalar);
    return std::pow(10.0f, dB / 20.0f);
}

inline bool IsSupportedSampleRate(double rate) {
    for (size_t i = 0; i < DFOVC_NUM_SAMPLE_RATES; ++i) {
        if (std::fabs(kDFOVCSupportedSampleRates[i] - rate) < 1.0) {
            return true;
        }
    }
    return false;
}

inline double NormalizeSampleRate(double rate) {
    for (size_t i = 0; i < DFOVC_NUM_SAMPLE_RATES; ++i) {
        if (std::fabs(kDFOVCSupportedSampleRates[i] - rate) < 1.0) {
            return kDFOVCSupportedSampleRates[i];
        }
    }
    return DFOVC_DEFAULT_SAMPLE_RATE;
}

inline uint32_t ClampBufferMs(uint32_t ms) {
    if (ms < DFOVC_MIN_BUFFER_MS) return DFOVC_MIN_BUFFER_MS;
    if (ms > DFOVC_MAX_BUFFER_MS) return DFOVC_MAX_BUFFER_MS;
    return ms;
}

inline uint32_t BufferFramesForConfig(double sampleRate, uint32_t bufferMs) {
    const double frames = std::ceil((sampleRate * static_cast<double>(bufferMs)) / 1000.0);
    return std::max<uint32_t>(static_cast<uint32_t>(frames), DFOVC_ZERO_TIMESTAMP_PERIOD);
}

inline AudioStreamBasicDescription MakeStreamFormat(double sampleRate) {
    AudioStreamBasicDescription desc = {};
    desc.mSampleRate       = sampleRate;
    desc.mFormatID         = kAudioFormatLinearPCM;
    desc.mFormatFlags      = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
    desc.mBytesPerPacket   = DFOVC_BYTES_PER_FRAME;
    desc.mFramesPerPacket  = 1;
    desc.mBytesPerFrame    = DFOVC_BYTES_PER_FRAME;
    desc.mChannelsPerFrame = DFOVC_CHANNELS;
    desc.mBitsPerChannel   = DFOVC_BITS_PER_SAMPLE;
    return desc;
}

// ============================================================================
// Per-Cable State
// ============================================================================

struct PendingConfigChange {
    bool     changeSampleRate = false;
    double   newSampleRate = DFOVC_DEFAULT_SAMPLE_RATE;
    bool     changeBufferMs = false;
    uint32_t newBufferMs = DFOVC_DEFAULT_BUFFER_MS;
};

struct CableState {
    uint32_t index = 0;
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

    // Configuration (mutated under mutex or during PerformDeviceConfigurationChange)
    double   sampleRate = DFOVC_DEFAULT_SAMPLE_RATE;
    uint32_t bufferDurationMs = DFOVC_DEFAULT_BUFFER_MS;

    // Realtime-accessible atomics (read without locking in IO callbacks)
    std::atomic<bool>     enabled{true};
    std::atomic<float>    outputVolumeScalar{1.0f};
    std::atomic<float>    inputVolumeScalar{1.0f};
    std::atomic<bool>     outputMute{false};
    std::atomic<bool>     inputMute{false};

    // Clock & IO lifecycle
    std::atomic<uint32_t> ioClientCount{0};
    std::atomic<uint64_t> anchorHostTime{0};
    std::atomic<uint64_t> clockSeed{1};
    std::atomic<double>   hostTicksPerFrame{0.0};

    // Lock-free loopback ring buffer
    dfovc::TimestampRingBuffer ring;

    void RecomputeClockTiming(const mach_timebase_info_data_t& timebase) {
        // hostTicksPerSecond = (1e9 * denom) / numer
        const double hostTicksPerSecond =
            (1000000000.0 * static_cast<double>(timebase.denom)) / static_cast<double>(timebase.numer);
        hostTicksPerFrame.store(hostTicksPerSecond / sampleRate, std::memory_order_release);
    }

    void ReconfigureRingBuffer() {
        const uint32_t frames = BufferFramesForConfig(sampleRate, bufferDurationMs);
        ring.Configure(frames, DFOVC_CHANNELS);
    }
};

// ============================================================================
// Global Plugin State
// ============================================================================

AudioServerPlugInHostRef gHost = nullptr;
std::atomic<ULONG> gRefCount{1};
mach_timebase_info_data_t gTimebase = {1, 1};
uint64_t gActiveWindowTicks = 0; // 300 ms in mach ticks
pthread_mutex_t gStorageMutex = PTHREAD_MUTEX_INITIALIZER;
CableState gCables[DFOVC_MAX_CABLES];

// Helper: convert milliseconds to mach_absolute_time ticks
uint64_t MillisecondsToHostTicks(uint64_t ms) {
    const double ns = static_cast<double>(ms) * 1000000.0;
    return static_cast<uint64_t>((ns * static_cast<double>(gTimebase.denom)) / static_cast<double>(gTimebase.numer));
}

// ============================================================================
// CFPropertyList Helpers & Persistence
// ============================================================================

void DictSetString(CFMutableDictionaryRef dict, const char* key, const char* value) {
    CFStringRef k = CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingUTF8);
    CFStringRef v = CFStringCreateWithCString(kCFAllocatorDefault, value, kCFStringEncodingUTF8);
    if (k && v) CFDictionarySetValue(dict, k, v);
    if (k) CFRelease(k);
    if (v) CFRelease(v);
}

void DictSetInt64(CFMutableDictionaryRef dict, const char* key, int64_t value) {
    CFStringRef k = CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingUTF8);
    CFNumberRef v = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &value);
    if (k && v) CFDictionarySetValue(dict, k, v);
    if (k) CFRelease(k);
    if (v) CFRelease(v);
}

void DictSetDouble(CFMutableDictionaryRef dict, const char* key, double value) {
    CFStringRef k = CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingUTF8);
    CFNumberRef v = CFNumberCreate(kCFAllocatorDefault, kCFNumberDoubleType, &value);
    if (k && v) CFDictionarySetValue(dict, k, v);
    if (k) CFRelease(k);
    if (v) CFRelease(v);
}

void DictSetBool(CFMutableDictionaryRef dict, const char* key, bool value) {
    CFStringRef k = CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingUTF8);
    if (k) {
        CFDictionarySetValue(dict, k, value ? kCFBooleanTrue : kCFBooleanFalse);
        CFRelease(k);
    }
}

bool DictGetDouble(CFDictionaryRef dict, const char* key, double& outVal) {
    CFStringRef k = CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingUTF8);
    if (!k) return false;
    CFTypeRef val = CFDictionaryGetValue(dict, k);
    CFRelease(k);
    if (val && CFGetTypeID(val) == CFNumberGetTypeID()) {
        return CFNumberGetValue(static_cast<CFNumberRef>(val), kCFNumberDoubleType, &outVal);
    }
    return false;
}

bool DictGetInt64(CFDictionaryRef dict, const char* key, int64_t& outVal) {
    CFStringRef k = CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingUTF8);
    if (!k) return false;
    CFTypeRef val = CFDictionaryGetValue(dict, k);
    CFRelease(k);
    if (val && CFGetTypeID(val) == CFNumberGetTypeID()) {
        return CFNumberGetValue(static_cast<CFNumberRef>(val), kCFNumberSInt64Type, &outVal);
    }
    return false;
}

bool DictGetBool(CFDictionaryRef dict, const char* key, bool& outVal) {
    CFStringRef k = CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingUTF8);
    if (!k) return false;
    CFTypeRef val = CFDictionaryGetValue(dict, k);
    CFRelease(k);
    if (!val) return false;
    if (CFGetTypeID(val) == CFBooleanGetTypeID()) {
        outVal = CFBooleanGetValue(static_cast<CFBooleanRef>(val));
        return true;
    }
    if (CFGetTypeID(val) == CFNumberGetTypeID()) {
        int32_t n = 0;
        if (CFNumberGetValue(static_cast<CFNumberRef>(val), kCFNumberSInt32Type, &n)) {
            outVal = (n != 0);
            return true;
        }
    }
    return false;
}

void SaveSettingsToStorage() {
    if (!gHost || !gHost->WriteToStorage) return;

    pthread_mutex_lock(&gStorageMutex);
    CFMutableArrayRef arr = CFArrayCreateMutable(kCFAllocatorDefault, DFOVC_MAX_CABLES, &kCFTypeArrayCallBacks);
    if (arr) {
        for (uint32_t i = 0; i < DFOVC_MAX_CABLES; ++i) {
            CableState& c = gCables[i];
            pthread_mutex_lock(&c.mutex);
            const double sr = c.sampleRate;
            const uint32_t bufMs = c.bufferDurationMs;
            pthread_mutex_unlock(&c.mutex);

            CFMutableDictionaryRef d = CFDictionaryCreateMutable(
                kCFAllocatorDefault, 8, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            if (d) {
                DictSetDouble(d, DFOVC_KEY_SAMPLE_RATE, sr);
                DictSetInt64(d, DFOVC_KEY_BUFFER_MS, static_cast<int64_t>(bufMs));
                DictSetBool(d, DFOVC_KEY_ENABLED, c.enabled.load(std::memory_order_relaxed));
                DictSetDouble(d, DFOVC_KEY_OUTPUT_VOLUME, c.outputVolumeScalar.load(std::memory_order_relaxed));
                DictSetDouble(d, DFOVC_KEY_INPUT_VOLUME, c.inputVolumeScalar.load(std::memory_order_relaxed));
                DictSetBool(d, DFOVC_KEY_OUTPUT_MUTE, c.outputMute.load(std::memory_order_relaxed));
                DictSetBool(d, DFOVC_KEY_INPUT_MUTE, c.inputMute.load(std::memory_order_relaxed));
                CFArrayAppendValue(arr, d);
                CFRelease(d);
            }
        }
        gHost->WriteToStorage(gHost, CFSTR(DFOVC_STORAGE_KEY), arr);
        CFRelease(arr);
    }
    pthread_mutex_unlock(&gStorageMutex);
}

void LoadSettingsFromStorage() {
    if (!gHost || !gHost->CopyFromStorage) return;

    CFPropertyListRef plist = nullptr;
    if (gHost->CopyFromStorage(gHost, CFSTR(DFOVC_STORAGE_KEY), &plist) != kAudioHardwareNoError || !plist) {
        return;
    }

    if (CFGetTypeID(plist) == CFArrayGetTypeID()) {
        CFArrayRef arr = static_cast<CFArrayRef>(plist);
        const CFIndex count = std::min<CFIndex>(CFArrayGetCount(arr), DFOVC_MAX_CABLES);
        for (CFIndex i = 0; i < count; ++i) {
            CFTypeRef item = CFArrayGetValueAtIndex(arr, i);
            if (!item || CFGetTypeID(item) != CFDictionaryGetTypeID()) continue;
            CFDictionaryRef d = static_cast<CFDictionaryRef>(item);
            CableState& c = gCables[i];

            double sr = 0.0;
            if (DictGetDouble(d, DFOVC_KEY_SAMPLE_RATE, sr) && IsSupportedSampleRate(sr)) {
                c.sampleRate = NormalizeSampleRate(sr);
            }
            int64_t bufMs = 0;
            if (DictGetInt64(d, DFOVC_KEY_BUFFER_MS, bufMs)) {
                c.bufferDurationMs = ClampBufferMs(static_cast<uint32_t>(bufMs));
            }
            bool bVal = true;
            if (DictGetBool(d, DFOVC_KEY_ENABLED, bVal)) {
                c.enabled.store(bVal, std::memory_order_relaxed);
            }
            double vol = 1.0;
            if (DictGetDouble(d, DFOVC_KEY_OUTPUT_VOLUME, vol)) {
                c.outputVolumeScalar.store(ClampScalar(static_cast<float>(vol)), std::memory_order_relaxed);
            }
            if (DictGetDouble(d, DFOVC_KEY_INPUT_VOLUME, vol)) {
                c.inputVolumeScalar.store(ClampScalar(static_cast<float>(vol)), std::memory_order_relaxed);
            }
            if (DictGetBool(d, DFOVC_KEY_OUTPUT_MUTE, bVal)) {
                c.outputMute.store(bVal, std::memory_order_relaxed);
            }
            if (DictGetBool(d, DFOVC_KEY_INPUT_MUTE, bVal)) {
                c.inputMute.store(bVal, std::memory_order_relaxed);
            }
        }
    }

    CFRelease(plist);
}

// ============================================================================
// Status Dictionary Builders
// ============================================================================

CFMutableDictionaryRef CreateCableStatusDictionary(uint32_t idx) {
    CableState& c = gCables[idx];
    const uint64_t now = mach_absolute_time();

    pthread_mutex_lock(&c.mutex);
    const double sr = c.sampleRate;
    const uint32_t bufMs = c.bufferDurationMs;
    const uint32_t bufBytes = c.ring.CapacityBytes();
    pthread_mutex_unlock(&c.mutex);

    const uint32_t ioClients = c.ioClientCount.load(std::memory_order_acquire);
    const bool renderActive = (ioClients > 0) && c.ring.IsWriterActive(now, gActiveWindowTicks);
    const bool captureActive = (ioClients > 0) && c.ring.IsReaderActive(now, gActiveWindowTicks);

    CFMutableDictionaryRef d = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 22, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (!d) return nullptr;

    DictSetInt64(d, DFOVC_KEY_CABLE_INDEX, static_cast<int64_t>(idx));
    DictSetString(d, DFOVC_KEY_NAME, kCableNames[idx]);
    DictSetString(d, DFOVC_KEY_UID, kCableUIDs[idx]);
    DictSetDouble(d, DFOVC_KEY_SAMPLE_RATE, sr);
    DictSetInt64(d, DFOVC_KEY_BITS_PER_SAMPLE, DFOVC_BITS_PER_SAMPLE);
    DictSetInt64(d, DFOVC_KEY_CHANNELS, DFOVC_CHANNELS);
    DictSetInt64(d, DFOVC_KEY_BUFFER_MS, static_cast<int64_t>(bufMs));
    DictSetInt64(d, DFOVC_KEY_BUFFER_BYTES, static_cast<int64_t>(bufBytes));
    DictSetInt64(d, DFOVC_KEY_BUFFER_FILL, static_cast<int64_t>(c.ring.CurrentFillBytes()));
    DictSetInt64(d, DFOVC_KEY_OVERRUNS, static_cast<int64_t>(c.ring.OverrunCount()));
    DictSetInt64(d, DFOVC_KEY_UNDERRUNS, static_cast<int64_t>(c.ring.UnderrunCount()));
    DictSetBool(d, DFOVC_KEY_RENDER_ACTIVE, renderActive);
    DictSetBool(d, DFOVC_KEY_CAPTURE_ACTIVE, captureActive);
    DictSetBool(d, DFOVC_KEY_ENABLED, c.enabled.load(std::memory_order_relaxed));
    DictSetDouble(d, DFOVC_KEY_OUTPUT_VOLUME, c.outputVolumeScalar.load(std::memory_order_relaxed));
    DictSetDouble(d, DFOVC_KEY_INPUT_VOLUME, c.inputVolumeScalar.load(std::memory_order_relaxed));
    DictSetBool(d, DFOVC_KEY_OUTPUT_MUTE, c.outputMute.load(std::memory_order_relaxed));
    DictSetBool(d, DFOVC_KEY_INPUT_MUTE, c.inputMute.load(std::memory_order_relaxed));
    DictSetDouble(d, DFOVC_KEY_OUTPUT_PEAK, renderActive ? c.ring.OutputPeak() : 0.0);
    DictSetDouble(d, DFOVC_KEY_INPUT_PEAK, captureActive ? c.ring.InputPeak() : 0.0);
    DictSetInt64(d, DFOVC_KEY_IO_CLIENTS, static_cast<int64_t>(ioClients));

    return d;
}

CFMutableDictionaryRef CreateAllStatusDictionary() {
    CFMutableDictionaryRef root = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 4, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (!root) return nullptr;

    DictSetString(root, DFOVC_KEY_VERSION, DFOVC_DRIVER_VERSION);
    DictSetInt64(root, DFOVC_KEY_CABLE_COUNT, DFOVC_MAX_CABLES);

    CFMutableArrayRef cablesArr = CFArrayCreateMutable(kCFAllocatorDefault, DFOVC_MAX_CABLES, &kCFTypeArrayCallBacks);
    if (cablesArr) {
        for (uint32_t i = 0; i < DFOVC_MAX_CABLES; ++i) {
            CFMutableDictionaryRef cd = CreateCableStatusDictionary(i);
            if (cd) {
                CFArrayAppendValue(cablesArr, cd);
                CFRelease(cd);
            }
        }
        CFStringRef key = CFSTR(DFOVC_KEY_CABLES);
        CFDictionarySetValue(root, key, cablesArr);
        CFRelease(cablesArr);
    }

    return root;
}

void NotifyPropertyChangedAsync(AudioObjectID objID, AudioObjectPropertySelector selector, AudioObjectPropertyScope scope) {
    if (!gHost || !gHost->PropertiesChanged) return;
    AudioObjectPropertyAddress addr = { selector, scope, kAudioObjectPropertyElementMain };
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{
        gHost->PropertiesChanged(gHost, objID, 1, &addr);
    });
}

void ScheduleDeviceConfigChange(uint32_t cableIdx, PendingConfigChange* change) {
    if (!gHost || !gHost->RequestDeviceConfigurationChange) {
        delete change;
        return;
    }
    const AudioObjectID devID = MakeObjectID(cableIdx, ObjectKind::Device);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{
        OSStatus err = gHost->RequestDeviceConfigurationChange(gHost, devID, 0, change);
        if (err != kAudioHardwareNoError) {
            delete change;
        }
    });
}

OSStatus ApplyCableConfigDictionary(uint32_t defaultCableIdx, bool allowIndexOverride, CFDictionaryRef dict) {
    uint32_t idx = defaultCableIdx;
    if (allowIndexOverride) {
        int64_t requestedIdx = -1;
        if (!DictGetInt64(dict, DFOVC_KEY_CABLE_INDEX, requestedIdx) ||
            requestedIdx < 0 || requestedIdx >= DFOVC_MAX_CABLES) {
            return kAudioHardwareIllegalOperationError;
        }
        idx = static_cast<uint32_t>(requestedIdx);
    }

    CableState& c = gCables[idx];
    bool persistNeeded = false;

    bool bVal = false;
    if (DictGetBool(dict, DFOVC_KEY_ENABLED, bVal)) {
        c.enabled.store(bVal, std::memory_order_release);
        persistNeeded = true;
    }

    double dVal = 0.0;
    if (DictGetDouble(dict, DFOVC_KEY_OUTPUT_VOLUME, dVal)) {
        c.outputVolumeScalar.store(ClampScalar(static_cast<float>(dVal)), std::memory_order_release);
        NotifyPropertyChangedAsync(MakeObjectID(idx, ObjectKind::VolumeOutput),
                                   kAudioLevelControlPropertyScalarValue,
                                   kAudioObjectPropertyScopeGlobal);
        NotifyPropertyChangedAsync(MakeObjectID(idx, ObjectKind::VolumeOutput),
                                   kAudioLevelControlPropertyDecibelValue,
                                   kAudioObjectPropertyScopeGlobal);
        persistNeeded = true;
    }
    if (DictGetDouble(dict, DFOVC_KEY_INPUT_VOLUME, dVal)) {
        c.inputVolumeScalar.store(ClampScalar(static_cast<float>(dVal)), std::memory_order_release);
        NotifyPropertyChangedAsync(MakeObjectID(idx, ObjectKind::VolumeInput),
                                   kAudioLevelControlPropertyScalarValue,
                                   kAudioObjectPropertyScopeGlobal);
        NotifyPropertyChangedAsync(MakeObjectID(idx, ObjectKind::VolumeInput),
                                   kAudioLevelControlPropertyDecibelValue,
                                   kAudioObjectPropertyScopeGlobal);
        persistNeeded = true;
    }
    if (DictGetBool(dict, DFOVC_KEY_OUTPUT_MUTE, bVal)) {
        c.outputMute.store(bVal, std::memory_order_release);
        NotifyPropertyChangedAsync(MakeObjectID(idx, ObjectKind::MuteOutput),
                                   kAudioBooleanControlPropertyValue,
                                   kAudioObjectPropertyScopeGlobal);
        persistNeeded = true;
    }
    if (DictGetBool(dict, DFOVC_KEY_INPUT_MUTE, bVal)) {
        c.inputMute.store(bVal, std::memory_order_release);
        NotifyPropertyChangedAsync(MakeObjectID(idx, ObjectKind::MuteInput),
                                   kAudioBooleanControlPropertyValue,
                                   kAudioObjectPropertyScopeGlobal);
        persistNeeded = true;
    }

    // Structural / IO-affecting changes (sample rate & ring buffer resize) MUST
    // go through RequestDeviceConfigurationChange so CoreAudio pauses IO cleanly.
    bool needsConfigChange = false;
    PendingConfigChange pending;

    pthread_mutex_lock(&c.mutex);
    const double curSR = c.sampleRate;
    const uint32_t curBufMs = c.bufferDurationMs;
    pthread_mutex_unlock(&c.mutex);

    if (DictGetDouble(dict, DFOVC_KEY_SAMPLE_RATE, dVal) && dVal > 0.0) {
        if (!IsSupportedSampleRate(dVal)) {
            return kAudioDeviceUnsupportedFormatError;
        }
        const double normSR = NormalizeSampleRate(dVal);
        if (std::fabs(normSR - curSR) >= 1.0) {
            pending.changeSampleRate = true;
            pending.newSampleRate = normSR;
            needsConfigChange = true;
        }
    }

    int64_t bufMsVal = 0;
    if (DictGetInt64(dict, DFOVC_KEY_BUFFER_MS, bufMsVal) && bufMsVal > 0) {
        const uint32_t clamped = ClampBufferMs(static_cast<uint32_t>(bufMsVal));
        if (clamped != curBufMs) {
            pending.changeBufferMs = true;
            pending.newBufferMs = clamped;
            needsConfigChange = true;
        }
    }

    if (persistNeeded && !needsConfigChange) {
        SaveSettingsToStorage();
    }

    if (needsConfigChange) {
        PendingConfigChange* heapChange = new (std::nothrow) PendingConfigChange(pending);
        if (!heapChange) return kAudioHardwareUnspecifiedError;
        ScheduleDeviceConfigChange(idx, heapChange);
    }

    return kAudioHardwareNoError;
}

// ============================================================================
// IUnknown Methods
// ============================================================================

extern AudioServerPlugInDriverInterface gDriverInterface;
AudioServerPlugInDriverInterface* gDriverInterfacePtr = &gDriverInterface;

HRESULT DFOVC_QueryInterface(void* /*inDriver*/, REFIID inUUID, LPVOID* outInterface) {
    if (!outInterface) return E_POINTER;

    CFUUIDRef requestedUUID = CFUUIDCreateFromUUIDBytes(kCFAllocatorDefault, inUUID);
    if (!requestedUUID) {
        *outInterface = nullptr;
        return E_NOINTERFACE;
    }

    HRESULT result = E_NOINTERFACE;
    if (CFEqual(requestedUUID, IUnknownUUID) ||
        CFEqual(requestedUUID, kAudioServerPlugInDriverInterfaceUUID)) {
        gRefCount.fetch_add(1, std::memory_order_relaxed);
        *outInterface = &gDriverInterfacePtr;
        result = S_OK;
    } else {
        *outInterface = nullptr;
    }

    CFRelease(requestedUUID);
    return result;
}

ULONG DFOVC_AddRef(void* /*inDriver*/) {
    return gRefCount.fetch_add(1, std::memory_order_relaxed) + 1;
}

ULONG DFOVC_Release(void* /*inDriver*/) {
    ULONG prev = gRefCount.load(std::memory_order_relaxed);
    while (prev > 1) {
        if (gRefCount.compare_exchange_weak(prev, prev - 1, std::memory_order_acq_rel)) {
            return prev - 1;
        }
    }
    return 1;
}

// ============================================================================
// Basic Operations
// ============================================================================

OSStatus DFOVC_Initialize(AudioServerPlugInDriverRef /*inDriver*/, AudioServerPlugInHostRef inHost) {
    gHost = inHost;
    mach_timebase_info(&gTimebase);
    gActiveWindowTicks = MillisecondsToHostTicks(300);

    for (uint32_t i = 0; i < DFOVC_MAX_CABLES; ++i) {
        gCables[i].index = i;
        gCables[i].sampleRate = DFOVC_DEFAULT_SAMPLE_RATE;
        gCables[i].bufferDurationMs = DFOVC_DEFAULT_BUFFER_MS;
        gCables[i].enabled.store(true, std::memory_order_relaxed);
        gCables[i].outputVolumeScalar.store(1.0f, std::memory_order_relaxed);
        gCables[i].inputVolumeScalar.store(1.0f, std::memory_order_relaxed);
        gCables[i].outputMute.store(false, std::memory_order_relaxed);
        gCables[i].inputMute.store(false, std::memory_order_relaxed);
        gCables[i].ioClientCount.store(0, std::memory_order_relaxed);
        gCables[i].clockSeed.store(1, std::memory_order_relaxed);
    }

    LoadSettingsFromStorage();

    for (uint32_t i = 0; i < DFOVC_MAX_CABLES; ++i) {
        gCables[i].RecomputeClockTiming(gTimebase);
        gCables[i].ReconfigureRingBuffer();
    }

    return kAudioHardwareNoError;
}

OSStatus DFOVC_CreateDevice(AudioServerPlugInDriverRef /*inDriver*/,
                            CFDictionaryRef /*inDescription*/,
                            const AudioServerPlugInClientInfo* /*inClientInfo*/,
                            AudioObjectID* /*outDeviceObjectID*/) {
    return kAudioHardwareUnsupportedOperationError;
}

OSStatus DFOVC_DestroyDevice(AudioServerPlugInDriverRef /*inDriver*/,
                             AudioObjectID /*inDeviceObjectID*/) {
    return kAudioHardwareUnsupportedOperationError;
}

OSStatus DFOVC_AddDeviceClient(AudioServerPlugInDriverRef /*inDriver*/,
                               AudioObjectID /*inDeviceObjectID*/,
                               const AudioServerPlugInClientInfo* /*inClientInfo*/) {
    return kAudioHardwareNoError;
}

OSStatus DFOVC_RemoveDeviceClient(AudioServerPlugInDriverRef /*inDriver*/,
                                  AudioObjectID /*inDeviceObjectID*/,
                                  const AudioServerPlugInClientInfo* /*inClientInfo*/) {
    return kAudioHardwareNoError;
}

OSStatus DFOVC_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef /*inDriver*/,
                                                AudioObjectID inDeviceObjectID,
                                                UInt64 /*inChangeAction*/,
                                                void* inChangeInfo) {
    uint32_t idx = 0;
    ObjectKind kind = ObjectKind::Invalid;
    PendingConfigChange* change = static_cast<PendingConfigChange*>(inChangeInfo);

    if (!DecodeObjectID(inDeviceObjectID, idx, kind) || kind != ObjectKind::Device) {
        delete change;
        return kAudioHardwareBadObjectError;
    }

    if (change) {
        CableState& c = gCables[idx];
        pthread_mutex_lock(&c.mutex);
        if (change->changeSampleRate && IsSupportedSampleRate(change->newSampleRate)) {
            c.sampleRate = NormalizeSampleRate(change->newSampleRate);
        }
        if (change->changeBufferMs) {
            c.bufferDurationMs = ClampBufferMs(change->newBufferMs);
        }
        c.RecomputeClockTiming(gTimebase);
        c.ReconfigureRingBuffer();
        c.clockSeed.fetch_add(1, std::memory_order_release);
        pthread_mutex_unlock(&c.mutex);

        SaveSettingsToStorage();
        delete change;
    }

    return kAudioHardwareNoError;
}

OSStatus DFOVC_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef /*inDriver*/,
                                              AudioObjectID /*inDeviceObjectID*/,
                                              UInt64 /*inChangeAction*/,
                                              void* inChangeInfo) {
    delete static_cast<PendingConfigChange*>(inChangeInfo);
    return kAudioHardwareNoError;
}

// ============================================================================
// Property Operations: PlugIn Object
// ============================================================================

Boolean PlugIn_HasProperty(const AudioObjectPropertyAddress* addr) {
    switch (addr->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyManufacturer:
        case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyDeviceList:
        case kAudioPlugInPropertyTranslateUIDToDevice:
        case kAudioPlugInPropertyResourceBundle:
        case kAudioObjectPropertyCustomPropertyInfoList:
        case kDFOVCPropertyAllStatus:
        case kDFOVCPropertyCableConfig:
        case kDFOVCPropertyResetStats:
            return true;
        default:
            return false;
    }
}

OSStatus PlugIn_IsPropertySettable(const AudioObjectPropertyAddress* addr, Boolean* outIsSettable) {
    switch (addr->mSelector) {
        case kDFOVCPropertyCableConfig:
        case kDFOVCPropertyResetStats:
            *outIsSettable = true;
            return kAudioHardwareNoError;
        default:
            *outIsSettable = false;
            return PlugIn_HasProperty(addr) ? kAudioHardwareNoError : kAudioHardwareUnknownPropertyError;
    }
}

OSStatus PlugIn_GetPropertyDataSize(const AudioObjectPropertyAddress* addr, UInt32* outDataSize) {
    switch (addr->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
            *outDataSize = sizeof(AudioClassID);
            return kAudioHardwareNoError;
        case kAudioObjectPropertyOwner:
            *outDataSize = sizeof(AudioObjectID);
            return kAudioHardwareNoError;
        case kAudioObjectPropertyManufacturer:
        case kAudioPlugInPropertyResourceBundle:
            *outDataSize = sizeof(CFStringRef);
            return kAudioHardwareNoError;
        case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyDeviceList:
            *outDataSize = DFOVC_MAX_CABLES * sizeof(AudioObjectID);
            return kAudioHardwareNoError;
        case kAudioPlugInPropertyTranslateUIDToDevice:
            *outDataSize = sizeof(AudioObjectID);
            return kAudioHardwareNoError;
        case kAudioObjectPropertyCustomPropertyInfoList:
            *outDataSize = sizeof(kPlugInCustomProperties);
            return kAudioHardwareNoError;
        case kDFOVCPropertyAllStatus:
        case kDFOVCPropertyCableConfig:
        case kDFOVCPropertyResetStats:
            *outDataSize = sizeof(CFPropertyListRef);
            return kAudioHardwareNoError;
        default:
            return kAudioHardwareUnknownPropertyError;
    }
}

OSStatus PlugIn_GetPropertyData(const AudioObjectPropertyAddress* addr,
                                UInt32 inQualifierDataSize,
                                const void* inQualifierData,
                                UInt32 inDataSize,
                                UInt32* outDataSize,
                                void* outData) {
    switch (addr->mSelector) {
        case kAudioObjectPropertyBaseClass:
            if (inDataSize < sizeof(AudioClassID)) return kAudioHardwareBadPropertySizeError;
            *static_cast<AudioClassID*>(outData) = kAudioObjectClassID;
            *outDataSize = sizeof(AudioClassID);
            return kAudioHardwareNoError;

        case kAudioObjectPropertyClass:
            if (inDataSize < sizeof(AudioClassID)) return kAudioHardwareBadPropertySizeError;
            *static_cast<AudioClassID*>(outData) = kAudioPlugInClassID;
            *outDataSize = sizeof(AudioClassID);
            return kAudioHardwareNoError;

        case kAudioObjectPropertyOwner:
            if (inDataSize < sizeof(AudioObjectID)) return kAudioHardwareBadPropertySizeError;
            *static_cast<AudioObjectID*>(outData) = kAudioObjectUnknown;
            *outDataSize = sizeof(AudioObjectID);
            return kAudioHardwareNoError;

        case kAudioObjectPropertyManufacturer:
            if (inDataSize < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
            *static_cast<CFStringRef*>(outData) = CFSTR(DFOVC_MANUFACTURER);
            CFRetain(*static_cast<CFStringRef*>(outData));
            *outDataSize = sizeof(CFStringRef);
            return kAudioHardwareNoError;

        case kAudioPlugInPropertyResourceBundle:
            if (inDataSize < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
            *static_cast<CFStringRef*>(outData) = CFSTR("");
            CFRetain(*static_cast<CFStringRef*>(outData));
            *outDataSize = sizeof(CFStringRef);
            return kAudioHardwareNoError;

        case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyDeviceList: {
            const uint32_t count = std::min<uint32_t>(inDataSize / sizeof(AudioObjectID), DFOVC_MAX_CABLES);
            AudioObjectID* ids = static_cast<AudioObjectID*>(outData);
            for (uint32_t i = 0; i < count; ++i) {
                ids[i] = MakeObjectID(i, ObjectKind::Device);
            }
            *outDataSize = count * sizeof(AudioObjectID);
            return kAudioHardwareNoError;
        }

        case kAudioPlugInPropertyTranslateUIDToDevice: {
            if (inDataSize < sizeof(AudioObjectID)) return kAudioHardwareBadPropertySizeError;
            if (inQualifierDataSize < sizeof(CFStringRef) || !inQualifierData) {
                return kAudioHardwareBadPropertySizeError;
            }
            CFStringRef uid = *static_cast<const CFStringRef*>(inQualifierData);
            AudioObjectID found = kAudioObjectUnknown;
            if (uid) {
                for (uint32_t i = 0; i < DFOVC_MAX_CABLES; ++i) {
                    CFStringRef candidate = CFStringCreateWithCString(kCFAllocatorDefault, kCableUIDs[i], kCFStringEncodingUTF8);
                    if (candidate) {
                        if (CFEqual(uid, candidate)) {
                            found = MakeObjectID(i, ObjectKind::Device);
                            CFRelease(candidate);
                            break;
                        }
                        CFRelease(candidate);
                    }
                }
            }
            *static_cast<AudioObjectID*>(outData) = found;
            *outDataSize = sizeof(AudioObjectID);
            return kAudioHardwareNoError;
        }

        case kAudioObjectPropertyCustomPropertyInfoList: {
            const UInt32 totalSize = sizeof(kPlugInCustomProperties);
            const UInt32 copySize = std::min(inDataSize, totalSize);
            std::memcpy(outData, kPlugInCustomProperties, copySize);
            *outDataSize = copySize;
            return kAudioHardwareNoError;
        }

        case kDFOVCPropertyAllStatus:
        case kDFOVCPropertyCableConfig:
        case kDFOVCPropertyResetStats: {
            if (inDataSize < sizeof(CFPropertyListRef)) return kAudioHardwareBadPropertySizeError;
            CFMutableDictionaryRef status = CreateAllStatusDictionary();
            if (!status) return kAudioHardwareUnspecifiedError;
            *static_cast<CFPropertyListRef*>(outData) = status;
            *outDataSize = sizeof(CFPropertyListRef);
            return kAudioHardwareNoError;
        }

        default:
            return kAudioHardwareUnknownPropertyError;
    }
}

OSStatus PlugIn_SetPropertyData(const AudioObjectPropertyAddress* addr,
                                UInt32 inDataSize,
                                const void* inData) {
    if (inDataSize < sizeof(CFPropertyListRef) || !inData) {
        return kAudioHardwareBadPropertySizeError;
    }
    CFPropertyListRef plist = *static_cast<const CFPropertyListRef*>(inData);
    if (!plist || CFGetTypeID(plist) != CFDictionaryGetTypeID()) {
        return kAudioHardwareIllegalOperationError;
    }
    CFDictionaryRef dict = static_cast<CFDictionaryRef>(plist);

    if (addr->mSelector == kDFOVCPropertyCableConfig) {
        return ApplyCableConfigDictionary(0, true, dict);
    }

    if (addr->mSelector == kDFOVCPropertyResetStats) {
        int64_t idx = -1;
        if (DictGetInt64(dict, DFOVC_KEY_CABLE_INDEX, idx)) {
            if (idx >= 0 && idx < DFOVC_MAX_CABLES) {
                gCables[idx].ring.ResetStats();
                return kAudioHardwareNoError;
            }
            return kAudioHardwareIllegalOperationError;
        }
        // No cableIndex specified -> reset all 5 cables
        for (uint32_t i = 0; i < DFOVC_MAX_CABLES; ++i) {
            gCables[i].ring.ResetStats();
        }
        return kAudioHardwareNoError;
    }

    return kAudioHardwareUnknownPropertyError;
}

// ============================================================================
// Property Operations: Device Object
// ============================================================================

Boolean Device_HasProperty(const AudioObjectPropertyAddress* addr) {
    switch (addr->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyName:
        case kAudioObjectPropertyManufacturer:
        case kAudioObjectPropertyOwnedObjects:
        case kAudioDevicePropertyDeviceUID:
        case kAudioDevicePropertyModelUID:
        case kAudioDevicePropertyTransportType:
        case kAudioDevicePropertyRelatedDevices:
        case kAudioDevicePropertyClockDomain:
        case kAudioDevicePropertyDeviceIsAlive:
        case kAudioDevicePropertyDeviceIsRunning:
        case kAudioDevicePropertyDeviceCanBeDefaultDevice:
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
        case kAudioDevicePropertyLatency:
        case kAudioDevicePropertyStreams:
        case kAudioObjectPropertyControlList:
        case kAudioDevicePropertySafetyOffset:
        case kAudioDevicePropertyNominalSampleRate:
        case kAudioDevicePropertyAvailableNominalSampleRates:
        case kAudioDevicePropertyIsHidden:
        case kAudioDevicePropertyPreferredChannelsForStereo:
        case kAudioDevicePropertyPreferredChannelLayout:
        case kAudioDevicePropertyZeroTimeStampPeriod:
        case kAudioDevicePropertyClockAlgorithm:
        case kAudioDevicePropertyClockIsStable:
        case kAudioObjectPropertyCustomPropertyInfoList:
        case kDFOVCPropertyCableStatus:
        case kDFOVCPropertyCableConfig:
        case kDFOVCPropertyResetStats:
            return true;
        default:
            return false;
    }
}

OSStatus Device_IsPropertySettable(const AudioObjectPropertyAddress* addr, Boolean* outIsSettable) {
    switch (addr->mSelector) {
        case kAudioDevicePropertyNominalSampleRate:
        case kDFOVCPropertyCableConfig:
        case kDFOVCPropertyResetStats:
            *outIsSettable = true;
            return kAudioHardwareNoError;
        default:
            *outIsSettable = false;
            return Device_HasProperty(addr) ? kAudioHardwareNoError : kAudioHardwareUnknownPropertyError;
    }
}

OSStatus Device_GetPropertyDataSize(uint32_t /*idx*/, const AudioObjectPropertyAddress* addr, UInt32* outDataSize) {
    switch (addr->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
            *outDataSize = sizeof(AudioClassID);
            return kAudioHardwareNoError;

        case kAudioObjectPropertyOwner:
        case kAudioDevicePropertyRelatedDevices:
            *outDataSize = sizeof(AudioObjectID);
            return kAudioHardwareNoError;

        case kAudioObjectPropertyName:
        case kAudioObjectPropertyManufacturer:
        case kAudioDevicePropertyDeviceUID:
        case kAudioDevicePropertyModelUID:
            *outDataSize = sizeof(CFStringRef);
            return kAudioHardwareNoError;

        case kAudioObjectPropertyOwnedObjects:
            if (addr->mScope == kAudioObjectPropertyScopeInput) {
                *outDataSize = 3 * sizeof(AudioObjectID); // StreamIn, VolIn, MuteIn
            } else if (addr->mScope == kAudioObjectPropertyScopeOutput) {
                *outDataSize = 3 * sizeof(AudioObjectID); // StreamOut, VolOut, MuteOut
            } else {
                *outDataSize = 6 * sizeof(AudioObjectID);
            }
            return kAudioHardwareNoError;

        case kAudioDevicePropertyStreams:
            if (addr->mScope == kAudioObjectPropertyScopeInput ||
                addr->mScope == kAudioObjectPropertyScopeOutput) {
                *outDataSize = 1 * sizeof(AudioObjectID);
            } else {
                *outDataSize = 2 * sizeof(AudioObjectID);
            }
            return kAudioHardwareNoError;

        case kAudioObjectPropertyControlList:
            if (addr->mScope == kAudioObjectPropertyScopeInput ||
                addr->mScope == kAudioObjectPropertyScopeOutput) {
                *outDataSize = 2 * sizeof(AudioObjectID);
            } else {
                *outDataSize = 4 * sizeof(AudioObjectID);
            }
            return kAudioHardwareNoError;

        case kAudioDevicePropertyTransportType:
        case kAudioDevicePropertyClockDomain:
        case kAudioDevicePropertyDeviceIsAlive:
        case kAudioDevicePropertyDeviceIsRunning:
        case kAudioDevicePropertyDeviceCanBeDefaultDevice:
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
        case kAudioDevicePropertyLatency:
        case kAudioDevicePropertySafetyOffset:
        case kAudioDevicePropertyIsHidden:
        case kAudioDevicePropertyZeroTimeStampPeriod:
        case kAudioDevicePropertyClockAlgorithm:
        case kAudioDevicePropertyClockIsStable:
            *outDataSize = sizeof(UInt32);
            return kAudioHardwareNoError;

        case kAudioDevicePropertyNominalSampleRate:
            *outDataSize = sizeof(Float64);
            return kAudioHardwareNoError;

        case kAudioDevicePropertyAvailableNominalSampleRates:
            *outDataSize = DFOVC_NUM_SAMPLE_RATES * sizeof(AudioValueRange);
            return kAudioHardwareNoError;

        case kAudioDevicePropertyPreferredChannelsForStereo:
            *outDataSize = 2 * sizeof(UInt32);
            return kAudioHardwareNoError;

        case kAudioDevicePropertyPreferredChannelLayout:
            *outDataSize = offsetof(AudioChannelLayout, mChannelDescriptions) + 2 * sizeof(AudioChannelDescription);
            return kAudioHardwareNoError;

        case kAudioObjectPropertyCustomPropertyInfoList:
            *outDataSize = sizeof(kDeviceCustomProperties);
            return kAudioHardwareNoError;

        case kDFOVCPropertyCableStatus:
        case kDFOVCPropertyCableConfig:
        case kDFOVCPropertyResetStats:
            *outDataSize = sizeof(CFPropertyListRef);
            return kAudioHardwareNoError;

        default:
            return kAudioHardwareUnknownPropertyError;
    }
}

OSStatus Device_GetPropertyData(uint32_t idx,
                                const AudioObjectPropertyAddress* addr,
                                UInt32 inDataSize,
                                UInt32* outDataSize,
                                void* outData) {
    CableState& c = gCables[idx];

    switch (addr->mSelector) {
        case kAudioObjectPropertyBaseClass:
            if (inDataSize < sizeof(AudioClassID)) return kAudioHardwareBadPropertySizeError;
            *static_cast<AudioClassID*>(outData) = kAudioObjectClassID;
            *outDataSize = sizeof(AudioClassID);
            return kAudioHardwareNoError;

        case kAudioObjectPropertyClass:
            if (inDataSize < sizeof(AudioClassID)) return kAudioHardwareBadPropertySizeError;
            *static_cast<AudioClassID*>(outData) = kAudioDeviceClassID;
            *outDataSize = sizeof(AudioClassID);
            return kAudioHardwareNoError;

        case kAudioObjectPropertyOwner:
            if (inDataSize < sizeof(AudioObjectID)) return kAudioHardwareBadPropertySizeError;
            *static_cast<AudioObjectID*>(outData) = kObjectID_PlugIn;
            *outDataSize = sizeof(AudioObjectID);
            return kAudioHardwareNoError;

        case kAudioObjectPropertyName:
            if (inDataSize < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
            *static_cast<CFStringRef*>(outData) = CFStringCreateWithCString(
                kCFAllocatorDefault, kCableNames[idx], kCFStringEncodingUTF8);
            *outDataSize = sizeof(CFStringRef);
            return kAudioHardwareNoError;

        case kAudioObjectPropertyManufacturer:
            if (inDataSize < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
            *static_cast<CFStringRef*>(outData) = CFSTR(DFOVC_MANUFACTURER);
            CFRetain(*static_cast<CFStringRef*>(outData));
            *outDataSize = sizeof(CFStringRef);
            return kAudioHardwareNoError;

        case kAudioDevicePropertyDeviceUID:
            if (inDataSize < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
            *static_cast<CFStringRef*>(outData) = CFStringCreateWithCString(
                kCFAllocatorDefault, kCableUIDs[idx], kCFStringEncodingUTF8);
            *outDataSize = sizeof(CFStringRef);
            return kAudioHardwareNoError;

        case kAudioDevicePropertyModelUID:
            if (inDataSize < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
            *static_cast<CFStringRef*>(outData) = CFSTR(DFOVC_MODEL_UID);
            CFRetain(*static_cast<CFStringRef*>(outData));
            *outDataSize = sizeof(CFStringRef);
            return kAudioHardwareNoError;

        case kAudioObjectPropertyOwnedObjects: {
            AudioObjectID all[6];
            uint32_t total = 0;
            if (addr->mScope == kAudioObjectPropertyScopeInput) {
                all[0] = MakeObjectID(idx, ObjectKind::StreamInput);
                all[1] = MakeObjectID(idx, ObjectKind::VolumeInput);
                all[2] = MakeObjectID(idx, ObjectKind::MuteInput);
                total = 3;
            } else if (addr->mScope == kAudioObjectPropertyScopeOutput) {
                all[0] = MakeObjectID(idx, ObjectKind::StreamOutput);
                all[1] = MakeObjectID(idx, ObjectKind::VolumeOutput);
                all[2] = MakeObjectID(idx, ObjectKind::MuteOutput);
                total = 3;
            } else {
                all[0] = MakeObjectID(idx, ObjectKind::StreamInput);
                all[1] = MakeObjectID(idx, ObjectKind::StreamOutput);
                all[2] = MakeObjectID(idx, ObjectKind::VolumeOutput);
                all[3] = MakeObjectID(idx, ObjectKind::MuteOutput);
                all[4] = MakeObjectID(idx, ObjectKind::VolumeInput);
                all[5] = MakeObjectID(idx, ObjectKind::MuteInput);
                total = 6;
            }
            const uint32_t count = std::min<uint32_t>(inDataSize / sizeof(AudioObjectID), total);
            std::memcpy(outData, all, count * sizeof(AudioObjectID));
            *outDataSize = count * sizeof(AudioObjectID);
            return kAudioHardwareNoError;
        }

        case kAudioDevicePropertyStreams: {
            AudioObjectID streams[2];
            uint32_t total = 0;
            if (addr->mScope == kAudioObjectPropertyScopeInput) {
                streams[0] = MakeObjectID(idx, ObjectKind::StreamInput);
                total = 1;
            } else if (addr->mScope == kAudioObjectPropertyScopeOutput) {
                streams[0] = MakeObjectID(idx, ObjectKind::StreamOutput);
                total = 1;
            } else {
                streams[0] = MakeObjectID(idx, ObjectKind::StreamInput);
                streams[1] = MakeObjectID(idx, ObjectKind::StreamOutput);
                total = 2;
            }
            const uint32_t count = std::min<uint32_t>(inDataSize / sizeof(AudioObjectID), total);
            std::memcpy(outData, streams, count * sizeof(AudioObjectID));
            *outDataSize = count * sizeof(AudioObjectID);
            return kAudioHardwareNoError;
        }

        case kAudioObjectPropertyControlList: {
            AudioObjectID ctrls[4];
            uint32_t total = 0;
            if (addr->mScope == kAudioObjectPropertyScopeInput) {
                ctrls[0] = MakeObjectID(idx, ObjectKind::VolumeInput);
                ctrls[1] = MakeObjectID(idx, ObjectKind::MuteInput);
                total = 2;
            } else if (addr->mScope == kAudioObjectPropertyScopeOutput) {
                ctrls[0] = MakeObjectID(idx, ObjectKind::VolumeOutput);
                ctrls[1] = MakeObjectID(idx, ObjectKind::MuteOutput);
                total = 2;
            } else {
                ctrls[0] = MakeObjectID(idx, ObjectKind::VolumeOutput);
                ctrls[1] = MakeObjectID(idx, ObjectKind::MuteOutput);
                ctrls[2] = MakeObjectID(idx, ObjectKind::VolumeInput);
                ctrls[3] = MakeObjectID(idx, ObjectKind::MuteInput);
                total = 4;
            }
            const uint32_t count = std::min<uint32_t>(inDataSize / sizeof(AudioObjectID), total);
            std::memcpy(outData, ctrls, count * sizeof(AudioObjectID));
            *outDataSize = count * sizeof(AudioObjectID);
            return kAudioHardwareNoError;
        }

        case kAudioDevicePropertyTransportType:
            if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
            *static_cast<UInt32*>(outData) = kAudioDeviceTransportTypeVirtual;
            *outDataSize = sizeof(UInt32);
            return kAudioHardwareNoError;

        case kAudioDevicePropertyRelatedDevices:
            if (inDataSize < sizeof(AudioObjectID)) return kAudioHardwareBadPropertySizeError;
            *static_cast<AudioObjectID*>(outData) = MakeObjectID(idx, ObjectKind::Device);
            *outDataSize = sizeof(AudioObjectID);
            return kAudioHardwareNoError;

        case kAudioDevicePropertyClockDomain:
            if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
            *static_cast<UInt32*>(outData) = 0;
            *outDataSize = sizeof(UInt32);
            return kAudioHardwareNoError;

        case kAudioDevicePropertyDeviceIsAlive:
            if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
            *static_cast<UInt32*>(outData) = 1;
            *outDataSize = sizeof(UInt32);
            return kAudioHardwareNoError;

        case kAudioDevicePropertyDeviceIsRunning:
            if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
            *static_cast<UInt32*>(outData) = (c.ioClientCount.load(std::memory_order_acquire) > 0) ? 1 : 0;
            *outDataSize = sizeof(UInt32);
            return kAudioHardwareNoError;

        case kAudioDevicePropertyDeviceCanBeDefaultDevice:
            if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
            *static_cast<UInt32*>(outData) = 1;
            *outDataSize = sizeof(UInt32);
            return kAudioHardwareNoError;

        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
            if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
            *static_cast<UInt32*>(outData) = (addr->mScope == kAudioObjectPropertyScopeInput) ? 0 : 1;
            *outDataSize = sizeof(UInt32);
            return kAudioHardwareNoError;

        case kAudioDevicePropertyLatency:
            if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
            *static_cast<UInt32*>(outData) = 0;
            *outDataSize = sizeof(UInt32);
            return kAudioHardwareNoError;

        case kAudioDevicePropertySafetyOffset:
            if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
            *static_cast<UInt32*>(outData) = DFOVC_SAFETY_OFFSET_FRAMES;
            *outDataSize = sizeof(UInt32);
            return kAudioHardwareNoError;

        case kAudioDevicePropertyNominalSampleRate:
            if (inDataSize < sizeof(Float64)) return kAudioHardwareBadPropertySizeError;
            pthread_mutex_lock(&c.mutex);
            *static_cast<Float64*>(outData) = c.sampleRate;
            pthread_mutex_unlock(&c.mutex);
            *outDataSize = sizeof(Float64);
            return kAudioHardwareNoError;

        case kAudioDevicePropertyAvailableNominalSampleRates: {
            const uint32_t count = std::min<uint32_t>(inDataSize / sizeof(AudioValueRange), DFOVC_NUM_SAMPLE_RATES);
            AudioValueRange* ranges = static_cast<AudioValueRange*>(outData);
            for (uint32_t i = 0; i < count; ++i) {
                ranges[i].mMinimum = kDFOVCSupportedSampleRates[i];
                ranges[i].mMaximum = kDFOVCSupportedSampleRates[i];
            }
            *outDataSize = count * sizeof(AudioValueRange);
            return kAudioHardwareNoError;
        }

        case kAudioDevicePropertyIsHidden:
            if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
            *static_cast<UInt32*>(outData) = 0;
            *outDataSize = sizeof(UInt32);
            return kAudioHardwareNoError;

        case kAudioDevicePropertyPreferredChannelsForStereo: {
            if (inDataSize < 2 * sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
            UInt32* ch = static_cast<UInt32*>(outData);
            ch[0] = 1;
            ch[1] = 2;
            *outDataSize = 2 * sizeof(UInt32);
            return kAudioHardwareNoError;
        }

        case kAudioDevicePropertyPreferredChannelLayout: {
            const UInt32 needed = offsetof(AudioChannelLayout, mChannelDescriptions) + 2 * sizeof(AudioChannelDescription);
            if (inDataSize < needed) return kAudioHardwareBadPropertySizeError;
            AudioChannelLayout* layout = static_cast<AudioChannelLayout*>(outData);
            std::memset(layout, 0, needed);
            layout->mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;
            layout->mNumberChannelDescriptions = 2;
            layout->mChannelDescriptions[0].mChannelLabel = kAudioChannelLabel_Left;
            layout->mChannelDescriptions[1].mChannelLabel = kAudioChannelLabel_Right;
            *outDataSize = needed;
            return kAudioHardwareNoError;
        }

        case kAudioDevicePropertyZeroTimeStampPeriod:
            if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
            *static_cast<UInt32*>(outData) = DFOVC_ZERO_TIMESTAMP_PERIOD;
            *outDataSize = sizeof(UInt32);
            return kAudioHardwareNoError;

        case kAudioDevicePropertyClockAlgorithm:
            if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
            *static_cast<UInt32*>(outData) = kAudioDeviceClockAlgorithmRaw;
            *outDataSize = sizeof(UInt32);
            return kAudioHardwareNoError;

        case kAudioDevicePropertyClockIsStable:
            if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
            *static_cast<UInt32*>(outData) = 1;
            *outDataSize = sizeof(UInt32);
            return kAudioHardwareNoError;

        case kAudioObjectPropertyCustomPropertyInfoList: {
            const UInt32 totalSize = sizeof(kDeviceCustomProperties);
            const UInt32 copySize = std::min(inDataSize, totalSize);
            std::memcpy(outData, kDeviceCustomProperties, copySize);
            *outDataSize = copySize;
            return kAudioHardwareNoError;
        }

        case kDFOVCPropertyCableStatus:
        case kDFOVCPropertyCableConfig:
        case kDFOVCPropertyResetStats: {
            if (inDataSize < sizeof(CFPropertyListRef)) return kAudioHardwareBadPropertySizeError;
            CFMutableDictionaryRef status = CreateCableStatusDictionary(idx);
            if (!status) return kAudioHardwareUnspecifiedError;
            *static_cast<CFPropertyListRef*>(outData) = status;
            *outDataSize = sizeof(CFPropertyListRef);
            return kAudioHardwareNoError;
        }

        default:
            return kAudioHardwareUnknownPropertyError;
    }
}

OSStatus Device_SetPropertyData(uint32_t idx,
                                const AudioObjectPropertyAddress* addr,
                                UInt32 inDataSize,
                                const void* inData) {
    CableState& c = gCables[idx];

    if (addr->mSelector == kAudioDevicePropertyNominalSampleRate) {
        if (inDataSize < sizeof(Float64) || !inData) return kAudioHardwareBadPropertySizeError;
        const Float64 requested = *static_cast<const Float64*>(inData);
        if (!IsSupportedSampleRate(requested)) {
            return kAudioDeviceUnsupportedFormatError;
        }
        const double normSR = NormalizeSampleRate(requested);
        pthread_mutex_lock(&c.mutex);
        const double curSR = c.sampleRate;
        pthread_mutex_unlock(&c.mutex);

        if (std::fabs(normSR - curSR) >= 1.0) {
            PendingConfigChange* change = new (std::nothrow) PendingConfigChange();
            if (!change) return kAudioHardwareUnspecifiedError;
            change->changeSampleRate = true;
            change->newSampleRate = normSR;
            ScheduleDeviceConfigChange(idx, change);
        }
        return kAudioHardwareNoError;
    }

    if (addr->mSelector == kDFOVCPropertyCableConfig) {
        if (inDataSize < sizeof(CFPropertyListRef) || !inData) return kAudioHardwareBadPropertySizeError;
        CFPropertyListRef plist = *static_cast<const CFPropertyListRef*>(inData);
        if (!plist || CFGetTypeID(plist) != CFDictionaryGetTypeID()) {
            return kAudioHardwareIllegalOperationError;
        }
        return ApplyCableConfigDictionary(idx, false, static_cast<CFDictionaryRef>(plist));
    }

    if (addr->mSelector == kDFOVCPropertyResetStats) {
        c.ring.ResetStats();
        return kAudioHardwareNoError;
    }

    return kAudioHardwareUnknownPropertyError;
}

// ============================================================================
// Property Operations: Stream Object
// ============================================================================

Boolean Stream_HasProperty(const AudioObjectPropertyAddress* addr) {
    switch (addr->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyName:
        case kAudioStreamPropertyIsActive:
        case kAudioStreamPropertyDirection:
        case kAudioStreamPropertyTerminalType:
        case kAudioStreamPropertyStartingChannel:
        case kAudioStreamPropertyLatency:
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat:
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats:
            return true;
        default:
            return false;
    }
}

OSStatus Stream_IsPropertySettable(const AudioObjectPropertyAddress* addr, Boolean* outIsSettable) {
    switch (addr->mSelector) {
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat:
            *outIsSettable = true;
            return kAudioHardwareNoError;
        default:
            *outIsSettable = false;
            return Stream_HasProperty(addr) ? kAudioHardwareNoError : kAudioHardwareUnknownPropertyError;
    }
}

OSStatus Stream_GetPropertyDataSize(const AudioObjectPropertyAddress* addr, UInt32* outDataSize) {
    switch (addr->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
            *outDataSize = sizeof(AudioClassID);
            return kAudioHardwareNoError;
        case kAudioObjectPropertyOwner:
            *outDataSize = sizeof(AudioObjectID);
            return kAudioHardwareNoError;
        case kAudioObjectPropertyName:
            *outDataSize = sizeof(CFStringRef);
            return kAudioHardwareNoError;
        case kAudioStreamPropertyIsActive:
        case kAudioStreamPropertyDirection:
        case kAudioStreamPropertyTerminalType:
        case kAudioStreamPropertyStartingChannel:
        case kAudioStreamPropertyLatency:
            *outDataSize = sizeof(UInt32);
            return kAudioHardwareNoError;
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat:
            *outDataSize = sizeof(AudioStreamBasicDescription);
            return kAudioHardwareNoError;
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats:
            *outDataSize = DFOVC_NUM_SAMPLE_RATES * sizeof(AudioStreamRangedDescription);
            return kAudioHardwareNoError;
        default:
            return kAudioHardwareUnknownPropertyError;
    }
}

OSStatus Stream_GetPropertyData(uint32_t idx,
                                bool isInput,
                                const AudioObjectPropertyAddress* addr,
                                UInt32 inDataSize,
                                UInt32* outDataSize,
                                void* outData) {
    CableState& c = gCables[idx];

    switch (addr->mSelector) {
        case kAudioObjectPropertyBaseClass:
            if (inDataSize < sizeof(AudioClassID)) return kAudioHardwareBadPropertySizeError;
            *static_cast<AudioClassID*>(outData) = kAudioObjectClassID;
            *outDataSize = sizeof(AudioClassID);
            return kAudioHardwareNoError;

        case kAudioObjectPropertyClass:
            if (inDataSize < sizeof(AudioClassID)) return kAudioHardwareBadPropertySizeError;
            *static_cast<AudioClassID*>(outData) = kAudioStreamClassID;
            *outDataSize = sizeof(AudioClassID);
            return kAudioHardwareNoError;

        case kAudioObjectPropertyOwner:
            if (inDataSize < sizeof(AudioObjectID)) return kAudioHardwareBadPropertySizeError;
            *static_cast<AudioObjectID*>(outData) = MakeObjectID(idx, ObjectKind::Device);
            *outDataSize = sizeof(AudioObjectID);
            return kAudioHardwareNoError;

        case kAudioObjectPropertyName: {
            if (inDataSize < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
            CFStringRef base = CFStringCreateWithCString(kCFAllocatorDefault, kCableNames[idx], kCFStringEncodingUTF8);
            CFStringRef full = CFStringCreateWithFormat(kCFAllocatorDefault, nullptr, CFSTR("%@ %s"),
                                                        base, isInput ? "Input" : "Output");
            if (base) CFRelease(base);
            *static_cast<CFStringRef*>(outData) = full;
            *outDataSize = sizeof(CFStringRef);
            return kAudioHardwareNoError;
        }

        case kAudioStreamPropertyIsActive:
            if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
            *static_cast<UInt32*>(outData) = 1;
            *outDataSize = sizeof(UInt32);
            return kAudioHardwareNoError;

        case kAudioStreamPropertyDirection:
            if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
            *static_cast<UInt32*>(outData) = isInput ? 1 : 0; // 1 = input, 0 = output
            *outDataSize = sizeof(UInt32);
            return kAudioHardwareNoError;

        case kAudioStreamPropertyTerminalType:
            if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
            *static_cast<UInt32*>(outData) = isInput ? kAudioStreamTerminalTypeMicrophone : kAudioStreamTerminalTypeSpeaker;
            *outDataSize = sizeof(UInt32);
            return kAudioHardwareNoError;

        case kAudioStreamPropertyStartingChannel:
            if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
            *static_cast<UInt32*>(outData) = 1;
            *outDataSize = sizeof(UInt32);
            return kAudioHardwareNoError;

        case kAudioStreamPropertyLatency:
            if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
            *static_cast<UInt32*>(outData) = 0;
            *outDataSize = sizeof(UInt32);
            return kAudioHardwareNoError;

        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat: {
            if (inDataSize < sizeof(AudioStreamBasicDescription)) return kAudioHardwareBadPropertySizeError;
            pthread_mutex_lock(&c.mutex);
            const double sr = c.sampleRate;
            pthread_mutex_unlock(&c.mutex);
            *static_cast<AudioStreamBasicDescription*>(outData) = MakeStreamFormat(sr);
            *outDataSize = sizeof(AudioStreamBasicDescription);
            return kAudioHardwareNoError;
        }

        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats: {
            const uint32_t count = std::min<uint32_t>(
                inDataSize / sizeof(AudioStreamRangedDescription), DFOVC_NUM_SAMPLE_RATES);
            AudioStreamRangedDescription* descs = static_cast<AudioStreamRangedDescription*>(outData);
            for (uint32_t i = 0; i < count; ++i) {
                descs[i].mFormat = MakeStreamFormat(kDFOVCSupportedSampleRates[i]);
                descs[i].mSampleRateRange.mMinimum = kDFOVCSupportedSampleRates[i];
                descs[i].mSampleRateRange.mMaximum = kDFOVCSupportedSampleRates[i];
            }
            *outDataSize = count * sizeof(AudioStreamRangedDescription);
            return kAudioHardwareNoError;
        }

        default:
            return kAudioHardwareUnknownPropertyError;
    }
}

OSStatus Stream_SetPropertyData(uint32_t idx,
                                const AudioObjectPropertyAddress* addr,
                                UInt32 inDataSize,
                                const void* inData) {
    if (addr->mSelector != kAudioStreamPropertyVirtualFormat &&
        addr->mSelector != kAudioStreamPropertyPhysicalFormat) {
        return kAudioHardwareUnknownPropertyError;
    }
    if (inDataSize < sizeof(AudioStreamBasicDescription) || !inData) {
        return kAudioHardwareBadPropertySizeError;
    }
    const AudioStreamBasicDescription* req = static_cast<const AudioStreamBasicDescription*>(inData);
    if (req->mFormatID != kAudioFormatLinearPCM ||
        req->mChannelsPerFrame != DFOVC_CHANNELS ||
        req->mBitsPerChannel != DFOVC_BITS_PER_SAMPLE ||
        !IsSupportedSampleRate(req->mSampleRate)) {
        return kAudioDeviceUnsupportedFormatError;
    }

    CableState& c = gCables[idx];
    const double normSR = NormalizeSampleRate(req->mSampleRate);
    pthread_mutex_lock(&c.mutex);
    const double curSR = c.sampleRate;
    pthread_mutex_unlock(&c.mutex);

    if (std::fabs(normSR - curSR) >= 1.0) {
        PendingConfigChange* change = new (std::nothrow) PendingConfigChange();
        if (!change) return kAudioHardwareUnspecifiedError;
        change->changeSampleRate = true;
        change->newSampleRate = normSR;
        ScheduleDeviceConfigChange(idx, change);
    }
    return kAudioHardwareNoError;
}

// ============================================================================
// Property Operations: Volume & Mute Controls
// ============================================================================

Boolean Volume_HasProperty(const AudioObjectPropertyAddress* addr) {
    switch (addr->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioControlPropertyScope:
        case kAudioControlPropertyElement:
        case kAudioLevelControlPropertyScalarValue:
        case kAudioLevelControlPropertyDecibelValue:
        case kAudioLevelControlPropertyDecibelRange:
        case kAudioLevelControlPropertyConvertScalarToDecibels:
        case kAudioLevelControlPropertyConvertDecibelsToScalar:
            return true;
        default:
            return false;
    }
}

OSStatus Volume_IsPropertySettable(const AudioObjectPropertyAddress* addr, Boolean* outIsSettable) {
    switch (addr->mSelector) {
        case kAudioLevelControlPropertyScalarValue:
        case kAudioLevelControlPropertyDecibelValue:
            *outIsSettable = true;
            return kAudioHardwareNoError;
        default:
            *outIsSettable = false;
            return Volume_HasProperty(addr) ? kAudioHardwareNoError : kAudioHardwareUnknownPropertyError;
    }
}

OSStatus Volume_GetPropertyDataSize(const AudioObjectPropertyAddress* addr, UInt32* outDataSize) {
    switch (addr->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
            *outDataSize = sizeof(AudioClassID);
            return kAudioHardwareNoError;
        case kAudioObjectPropertyOwner:
            *outDataSize = sizeof(AudioObjectID);
            return kAudioHardwareNoError;
        case kAudioControlPropertyScope:
        case kAudioControlPropertyElement:
            *outDataSize = sizeof(UInt32);
            return kAudioHardwareNoError;
        case kAudioLevelControlPropertyScalarValue:
        case kAudioLevelControlPropertyDecibelValue:
        case kAudioLevelControlPropertyConvertScalarToDecibels:
        case kAudioLevelControlPropertyConvertDecibelsToScalar:
            *outDataSize = sizeof(Float32);
            return kAudioHardwareNoError;
        case kAudioLevelControlPropertyDecibelRange:
            *outDataSize = sizeof(AudioValueRange);
            return kAudioHardwareNoError;
        default:
            return kAudioHardwareUnknownPropertyError;
    }
}

OSStatus Volume_GetPropertyData(uint32_t idx,
                                bool isInput,
                                const AudioObjectPropertyAddress* addr,
                                UInt32 inDataSize,
                                UInt32* outDataSize,
                                void* outData) {
    CableState& c = gCables[idx];
    const float scalar = isInput
        ? c.inputVolumeScalar.load(std::memory_order_acquire)
        : c.outputVolumeScalar.load(std::memory_order_acquire);

    switch (addr->mSelector) {
        case kAudioObjectPropertyBaseClass:
            if (inDataSize < sizeof(AudioClassID)) return kAudioHardwareBadPropertySizeError;
            *static_cast<AudioClassID*>(outData) = kAudioLevelControlClassID;
            *outDataSize = sizeof(AudioClassID);
            return kAudioHardwareNoError;

        case kAudioObjectPropertyClass:
            if (inDataSize < sizeof(AudioClassID)) return kAudioHardwareBadPropertySizeError;
            *static_cast<AudioClassID*>(outData) = kAudioVolumeControlClassID;
            *outDataSize = sizeof(AudioClassID);
            return kAudioHardwareNoError;

        case kAudioObjectPropertyOwner:
            if (inDataSize < sizeof(AudioObjectID)) return kAudioHardwareBadPropertySizeError;
            *static_cast<AudioObjectID*>(outData) = MakeObjectID(idx, ObjectKind::Device);
            *outDataSize = sizeof(AudioObjectID);
            return kAudioHardwareNoError;

        case kAudioControlPropertyScope:
            if (inDataSize < sizeof(AudioObjectPropertyScope)) return kAudioHardwareBadPropertySizeError;
            *static_cast<AudioObjectPropertyScope*>(outData) =
                isInput ? kAudioObjectPropertyScopeInput : kAudioObjectPropertyScopeOutput;
            *outDataSize = sizeof(AudioObjectPropertyScope);
            return kAudioHardwareNoError;

        case kAudioControlPropertyElement:
            if (inDataSize < sizeof(AudioObjectPropertyElement)) return kAudioHardwareBadPropertySizeError;
            *static_cast<AudioObjectPropertyElement*>(outData) = kAudioObjectPropertyElementMain;
            *outDataSize = sizeof(AudioObjectPropertyElement);
            return kAudioHardwareNoError;

        case kAudioLevelControlPropertyScalarValue:
            if (inDataSize < sizeof(Float32)) return kAudioHardwareBadPropertySizeError;
            *static_cast<Float32*>(outData) = scalar;
            *outDataSize = sizeof(Float32);
            return kAudioHardwareNoError;

        case kAudioLevelControlPropertyDecibelValue:
            if (inDataSize < sizeof(Float32)) return kAudioHardwareBadPropertySizeError;
            *static_cast<Float32*>(outData) = ScalarToDecibels(scalar);
            *outDataSize = sizeof(Float32);
            return kAudioHardwareNoError;

        case kAudioLevelControlPropertyDecibelRange: {
            if (inDataSize < sizeof(AudioValueRange)) return kAudioHardwareBadPropertySizeError;
            AudioValueRange* r = static_cast<AudioValueRange*>(outData);
            r->mMinimum = kVolumeMinDB;
            r->mMaximum = kVolumeMaxDB;
            *outDataSize = sizeof(AudioValueRange);
            return kAudioHardwareNoError;
        }

        case kAudioLevelControlPropertyConvertScalarToDecibels: {
            if (inDataSize < sizeof(Float32)) return kAudioHardwareBadPropertySizeError;
            Float32* val = static_cast<Float32*>(outData);
            *val = ScalarToDecibels(*val);
            *outDataSize = sizeof(Float32);
            return kAudioHardwareNoError;
        }

        case kAudioLevelControlPropertyConvertDecibelsToScalar: {
            if (inDataSize < sizeof(Float32)) return kAudioHardwareBadPropertySizeError;
            Float32* val = static_cast<Float32*>(outData);
            *val = DecibelsToScalar(*val);
            *outDataSize = sizeof(Float32);
            return kAudioHardwareNoError;
        }

        default:
            return kAudioHardwareUnknownPropertyError;
    }
}

OSStatus Volume_SetPropertyData(uint32_t idx,
                                bool isInput,
                                const AudioObjectPropertyAddress* addr,
                                UInt32 inDataSize,
                                const void* inData) {
    if (inDataSize < sizeof(Float32) || !inData) return kAudioHardwareBadPropertySizeError;
    float newScalar = 1.0f;
    if (addr->mSelector == kAudioLevelControlPropertyScalarValue) {
        newScalar = ClampScalar(*static_cast<const Float32*>(inData));
    } else if (addr->mSelector == kAudioLevelControlPropertyDecibelValue) {
        newScalar = DecibelsToScalar(*static_cast<const Float32*>(inData));
    } else {
        return kAudioHardwareUnknownPropertyError;
    }

    CableState& c = gCables[idx];
    if (isInput) {
        c.inputVolumeScalar.store(newScalar, std::memory_order_release);
    } else {
        c.outputVolumeScalar.store(newScalar, std::memory_order_release);
    }

    const AudioObjectID ctrlID = MakeObjectID(idx, isInput ? ObjectKind::VolumeInput : ObjectKind::VolumeOutput);
    NotifyPropertyChangedAsync(ctrlID, kAudioLevelControlPropertyScalarValue, kAudioObjectPropertyScopeGlobal);
    NotifyPropertyChangedAsync(ctrlID, kAudioLevelControlPropertyDecibelValue, kAudioObjectPropertyScopeGlobal);
    SaveSettingsToStorage();
    return kAudioHardwareNoError;
}

Boolean Mute_HasProperty(const AudioObjectPropertyAddress* addr) {
    switch (addr->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioControlPropertyScope:
        case kAudioControlPropertyElement:
        case kAudioBooleanControlPropertyValue:
            return true;
        default:
            return false;
    }
}

OSStatus Mute_IsPropertySettable(const AudioObjectPropertyAddress* addr, Boolean* outIsSettable) {
    if (addr->mSelector == kAudioBooleanControlPropertyValue) {
        *outIsSettable = true;
        return kAudioHardwareNoError;
    }
    *outIsSettable = false;
    return Mute_HasProperty(addr) ? kAudioHardwareNoError : kAudioHardwareUnknownPropertyError;
}

OSStatus Mute_GetPropertyDataSize(const AudioObjectPropertyAddress* addr, UInt32* outDataSize) {
    switch (addr->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
            *outDataSize = sizeof(AudioClassID);
            return kAudioHardwareNoError;
        case kAudioObjectPropertyOwner:
            *outDataSize = sizeof(AudioObjectID);
            return kAudioHardwareNoError;
        case kAudioControlPropertyScope:
        case kAudioControlPropertyElement:
        case kAudioBooleanControlPropertyValue:
            *outDataSize = sizeof(UInt32);
            return kAudioHardwareNoError;
        default:
            return kAudioHardwareUnknownPropertyError;
    }
}

OSStatus Mute_GetPropertyData(uint32_t idx,
                              bool isInput,
                              const AudioObjectPropertyAddress* addr,
                              UInt32 inDataSize,
                              UInt32* outDataSize,
                              void* outData) {
    CableState& c = gCables[idx];
    switch (addr->mSelector) {
        case kAudioObjectPropertyBaseClass:
            if (inDataSize < sizeof(AudioClassID)) return kAudioHardwareBadPropertySizeError;
            *static_cast<AudioClassID*>(outData) = kAudioBooleanControlClassID;
            *outDataSize = sizeof(AudioClassID);
            return kAudioHardwareNoError;

        case kAudioObjectPropertyClass:
            if (inDataSize < sizeof(AudioClassID)) return kAudioHardwareBadPropertySizeError;
            *static_cast<AudioClassID*>(outData) = kAudioMuteControlClassID;
            *outDataSize = sizeof(AudioClassID);
            return kAudioHardwareNoError;

        case kAudioObjectPropertyOwner:
            if (inDataSize < sizeof(AudioObjectID)) return kAudioHardwareBadPropertySizeError;
            *static_cast<AudioObjectID*>(outData) = MakeObjectID(idx, ObjectKind::Device);
            *outDataSize = sizeof(AudioObjectID);
            return kAudioHardwareNoError;

        case kAudioControlPropertyScope:
            if (inDataSize < sizeof(AudioObjectPropertyScope)) return kAudioHardwareBadPropertySizeError;
            *static_cast<AudioObjectPropertyScope*>(outData) =
                isInput ? kAudioObjectPropertyScopeInput : kAudioObjectPropertyScopeOutput;
            *outDataSize = sizeof(AudioObjectPropertyScope);
            return kAudioHardwareNoError;

        case kAudioControlPropertyElement:
            if (inDataSize < sizeof(AudioObjectPropertyElement)) return kAudioHardwareBadPropertySizeError;
            *static_cast<AudioObjectPropertyElement*>(outData) = kAudioObjectPropertyElementMain;
            *outDataSize = sizeof(AudioObjectPropertyElement);
            return kAudioHardwareNoError;

        case kAudioBooleanControlPropertyValue: {
            if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
            const bool muted = isInput
                ? c.inputMute.load(std::memory_order_acquire)
                : c.outputMute.load(std::memory_order_acquire);
            *static_cast<UInt32*>(outData) = muted ? 1 : 0;
            *outDataSize = sizeof(UInt32);
            return kAudioHardwareNoError;
        }

        default:
            return kAudioHardwareUnknownPropertyError;
    }
}

OSStatus Mute_SetPropertyData(uint32_t idx,
                              bool isInput,
                              const AudioObjectPropertyAddress* addr,
                              UInt32 inDataSize,
                              const void* inData) {
    if (addr->mSelector != kAudioBooleanControlPropertyValue) {
        return kAudioHardwareUnknownPropertyError;
    }
    if (inDataSize < sizeof(UInt32) || !inData) return kAudioHardwareBadPropertySizeError;
    const bool muted = (*static_cast<const UInt32*>(inData)) != 0;

    CableState& c = gCables[idx];
    if (isInput) {
        c.inputMute.store(muted, std::memory_order_release);
    } else {
        c.outputMute.store(muted, std::memory_order_release);
    }

    const AudioObjectID ctrlID = MakeObjectID(idx, isInput ? ObjectKind::MuteInput : ObjectKind::MuteOutput);
    NotifyPropertyChangedAsync(ctrlID, kAudioBooleanControlPropertyValue, kAudioObjectPropertyScopeGlobal);
    SaveSettingsToStorage();
    return kAudioHardwareNoError;
}

// ============================================================================
// Top-Level Property Dispatchers
// ============================================================================

Boolean DFOVC_HasProperty(AudioServerPlugInDriverRef /*inDriver*/,
                          AudioObjectID inObjectID,
                          pid_t /*inClientProcessID*/,
                          const AudioObjectPropertyAddress* inAddress) {
    if (!inAddress) return false;
    if (inObjectID == kObjectID_PlugIn) return PlugIn_HasProperty(inAddress);

    uint32_t idx = 0;
    ObjectKind kind = ObjectKind::Invalid;
    if (!DecodeObjectID(inObjectID, idx, kind)) return false;

    switch (kind) {
        case ObjectKind::Device:       return Device_HasProperty(inAddress);
        case ObjectKind::StreamInput:
        case ObjectKind::StreamOutput: return Stream_HasProperty(inAddress);
        case ObjectKind::VolumeOutput:
        case ObjectKind::VolumeInput:  return Volume_HasProperty(inAddress);
        case ObjectKind::MuteOutput:
        case ObjectKind::MuteInput:    return Mute_HasProperty(inAddress);
        default:                       return false;
    }
}

OSStatus DFOVC_IsPropertySettable(AudioServerPlugInDriverRef /*inDriver*/,
                                  AudioObjectID inObjectID,
                                  pid_t /*inClientProcessID*/,
                                  const AudioObjectPropertyAddress* inAddress,
                                  Boolean* outIsSettable) {
    if (!inAddress || !outIsSettable) return kAudioHardwareIllegalOperationError;
    if (inObjectID == kObjectID_PlugIn) return PlugIn_IsPropertySettable(inAddress, outIsSettable);

    uint32_t idx = 0;
    ObjectKind kind = ObjectKind::Invalid;
    if (!DecodeObjectID(inObjectID, idx, kind)) return kAudioHardwareBadObjectError;

    switch (kind) {
        case ObjectKind::Device:       return Device_IsPropertySettable(inAddress, outIsSettable);
        case ObjectKind::StreamInput:
        case ObjectKind::StreamOutput: return Stream_IsPropertySettable(inAddress, outIsSettable);
        case ObjectKind::VolumeOutput:
        case ObjectKind::VolumeInput:  return Volume_IsPropertySettable(inAddress, outIsSettable);
        case ObjectKind::MuteOutput:
        case ObjectKind::MuteInput:    return Mute_IsPropertySettable(inAddress, outIsSettable);
        default:                       return kAudioHardwareBadObjectError;
    }
}

OSStatus DFOVC_GetPropertyDataSize(AudioServerPlugInDriverRef /*inDriver*/,
                                   AudioObjectID inObjectID,
                                   pid_t /*inClientProcessID*/,
                                   const AudioObjectPropertyAddress* inAddress,
                                   UInt32 /*inQualifierDataSize*/,
                                   const void* /*inQualifierData*/,
                                   UInt32* outDataSize) {
    if (!inAddress || !outDataSize) return kAudioHardwareIllegalOperationError;
    if (inObjectID == kObjectID_PlugIn) return PlugIn_GetPropertyDataSize(inAddress, outDataSize);

    uint32_t idx = 0;
    ObjectKind kind = ObjectKind::Invalid;
    if (!DecodeObjectID(inObjectID, idx, kind)) return kAudioHardwareBadObjectError;

    switch (kind) {
        case ObjectKind::Device:       return Device_GetPropertyDataSize(idx, inAddress, outDataSize);
        case ObjectKind::StreamInput:
        case ObjectKind::StreamOutput: return Stream_GetPropertyDataSize(inAddress, outDataSize);
        case ObjectKind::VolumeOutput:
        case ObjectKind::VolumeInput:  return Volume_GetPropertyDataSize(inAddress, outDataSize);
        case ObjectKind::MuteOutput:
        case ObjectKind::MuteInput:    return Mute_GetPropertyDataSize(inAddress, outDataSize);
        default:                       return kAudioHardwareBadObjectError;
    }
}

OSStatus DFOVC_GetPropertyData(AudioServerPlugInDriverRef /*inDriver*/,
                               AudioObjectID inObjectID,
                               pid_t /*inClientProcessID*/,
                               const AudioObjectPropertyAddress* inAddress,
                               UInt32 inQualifierDataSize,
                               const void* inQualifierData,
                               UInt32 inDataSize,
                               UInt32* outDataSize,
                               void* outData) {
    if (!inAddress || !outDataSize || !outData) return kAudioHardwareIllegalOperationError;
    if (inObjectID == kObjectID_PlugIn) {
        return PlugIn_GetPropertyData(inAddress, inQualifierDataSize, inQualifierData, inDataSize, outDataSize, outData);
    }

    uint32_t idx = 0;
    ObjectKind kind = ObjectKind::Invalid;
    if (!DecodeObjectID(inObjectID, idx, kind)) return kAudioHardwareBadObjectError;

    switch (kind) {
        case ObjectKind::Device:       return Device_GetPropertyData(idx, inAddress, inDataSize, outDataSize, outData);
        case ObjectKind::StreamInput:  return Stream_GetPropertyData(idx, true, inAddress, inDataSize, outDataSize, outData);
        case ObjectKind::StreamOutput: return Stream_GetPropertyData(idx, false, inAddress, inDataSize, outDataSize, outData);
        case ObjectKind::VolumeOutput: return Volume_GetPropertyData(idx, false, inAddress, inDataSize, outDataSize, outData);
        case ObjectKind::VolumeInput:  return Volume_GetPropertyData(idx, true, inAddress, inDataSize, outDataSize, outData);
        case ObjectKind::MuteOutput:   return Mute_GetPropertyData(idx, false, inAddress, inDataSize, outDataSize, outData);
        case ObjectKind::MuteInput:    return Mute_GetPropertyData(idx, true, inAddress, inDataSize, outDataSize, outData);
        default:                       return kAudioHardwareBadObjectError;
    }
}

OSStatus DFOVC_SetPropertyData(AudioServerPlugInDriverRef /*inDriver*/,
                               AudioObjectID inObjectID,
                               pid_t /*inClientProcessID*/,
                               const AudioObjectPropertyAddress* inAddress,
                               UInt32 /*inQualifierDataSize*/,
                               const void* /*inQualifierData*/,
                               UInt32 inDataSize,
                               const void* inData) {
    if (!inAddress) return kAudioHardwareIllegalOperationError;
    if (inObjectID == kObjectID_PlugIn) return PlugIn_SetPropertyData(inAddress, inDataSize, inData);

    uint32_t idx = 0;
    ObjectKind kind = ObjectKind::Invalid;
    if (!DecodeObjectID(inObjectID, idx, kind)) return kAudioHardwareBadObjectError;

    switch (kind) {
        case ObjectKind::Device:       return Device_SetPropertyData(idx, inAddress, inDataSize, inData);
        case ObjectKind::StreamInput:
        case ObjectKind::StreamOutput: return Stream_SetPropertyData(idx, inAddress, inDataSize, inData);
        case ObjectKind::VolumeOutput: return Volume_SetPropertyData(idx, false, inAddress, inDataSize, inData);
        case ObjectKind::VolumeInput:  return Volume_SetPropertyData(idx, true, inAddress, inDataSize, inData);
        case ObjectKind::MuteOutput:   return Mute_SetPropertyData(idx, false, inAddress, inDataSize, inData);
        case ObjectKind::MuteInput:    return Mute_SetPropertyData(idx, true, inAddress, inDataSize, inData);
        default:                       return kAudioHardwareBadObjectError;
    }
}

// ============================================================================
// Realtime IO Operations
// ============================================================================

OSStatus DFOVC_StartIO(AudioServerPlugInDriverRef /*inDriver*/,
                       AudioObjectID inDeviceObjectID,
                       UInt32 /*inClientID*/) {
    uint32_t idx = 0;
    ObjectKind kind = ObjectKind::Invalid;
    if (!DecodeObjectID(inDeviceObjectID, idx, kind) || kind != ObjectKind::Device) {
        return kAudioHardwareBadObjectError;
    }

    CableState& c = gCables[idx];
    pthread_mutex_lock(&c.mutex);
    const uint32_t prev = c.ioClientCount.fetch_add(1, std::memory_order_acq_rel);
    if (prev == 0) {
        c.ring.ResetStreamState();
        c.anchorHostTime.store(mach_absolute_time(), std::memory_order_release);
        c.clockSeed.fetch_add(1, std::memory_order_release);
    }
    pthread_mutex_unlock(&c.mutex);

    return kAudioHardwareNoError;
}

OSStatus DFOVC_StopIO(AudioServerPlugInDriverRef /*inDriver*/,
                      AudioObjectID inDeviceObjectID,
                      UInt32 /*inClientID*/) {
    uint32_t idx = 0;
    ObjectKind kind = ObjectKind::Invalid;
    if (!DecodeObjectID(inDeviceObjectID, idx, kind) || kind != ObjectKind::Device) {
        return kAudioHardwareBadObjectError;
    }

    CableState& c = gCables[idx];
    pthread_mutex_lock(&c.mutex);
    const uint32_t cur = c.ioClientCount.load(std::memory_order_acquire);
    if (cur > 0) {
        const uint32_t remaining = c.ioClientCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (remaining == 0) {
            c.ring.ResetStreamState();
        }
    }
    pthread_mutex_unlock(&c.mutex);

    return kAudioHardwareNoError;
}

OSStatus DFOVC_GetZeroTimeStamp(AudioServerPlugInDriverRef /*inDriver*/,
                                AudioObjectID inDeviceObjectID,
                                UInt32 /*inClientID*/,
                                Float64* outSampleTime,
                                UInt64* outHostTime,
                                UInt64* outSeed) {
    if (!outSampleTime || !outHostTime || !outSeed) return kAudioHardwareIllegalOperationError;

    uint32_t idx = 0;
    ObjectKind kind = ObjectKind::Invalid;
    if (!DecodeObjectID(inDeviceObjectID, idx, kind) || kind != ObjectKind::Device) {
        return kAudioHardwareBadObjectError;
    }

    CableState& c = gCables[idx];
    const uint64_t anchor = c.anchorHostTime.load(std::memory_order_acquire);
    const double ticksPerFrame = c.hostTicksPerFrame.load(std::memory_order_acquire);
    const uint64_t seed = c.clockSeed.load(std::memory_order_acquire);

    const uint64_t now = mach_absolute_time();
    const double ticksPerRing = ticksPerFrame * static_cast<double>(DFOVC_ZERO_TIMESTAMP_PERIOD);
    const uint64_t elapsed = (now >= anchor) ? (now - anchor) : 0;
    const uint64_t rings = (ticksPerRing > 0.0) ? static_cast<uint64_t>(static_cast<double>(elapsed) / ticksPerRing) : 0;

    *outSampleTime = static_cast<Float64>(rings * static_cast<uint64_t>(DFOVC_ZERO_TIMESTAMP_PERIOD));
    *outHostTime   = anchor + static_cast<UInt64>(static_cast<double>(rings) * ticksPerRing);
    *outSeed       = seed;

    return kAudioHardwareNoError;
}

OSStatus DFOVC_WillDoIOOperation(AudioServerPlugInDriverRef /*inDriver*/,
                                 AudioObjectID inDeviceObjectID,
                                 UInt32 /*inClientID*/,
                                 UInt32 inOperationID,
                                 Boolean* outWillDo,
                                 Boolean* outWillDoInPlace) {
    uint32_t idx = 0;
    ObjectKind kind = ObjectKind::Invalid;
    if (!DecodeObjectID(inDeviceObjectID, idx, kind) || kind != ObjectKind::Device) {
        return kAudioHardwareBadObjectError;
    }
    if (!outWillDo || !outWillDoInPlace) return kAudioHardwareIllegalOperationError;

    switch (inOperationID) {
        case kAudioServerPlugInIOOperationReadInput:
        case kAudioServerPlugInIOOperationWriteMix:
            *outWillDo = true;
            *outWillDoInPlace = true;
            break;
        default:
            *outWillDo = false;
            *outWillDoInPlace = true;
            break;
    }

    return kAudioHardwareNoError;
}

OSStatus DFOVC_BeginIOOperation(AudioServerPlugInDriverRef /*inDriver*/,
                                AudioObjectID /*inDeviceObjectID*/,
                                UInt32 /*inClientID*/,
                                UInt32 /*inOperationID*/,
                                UInt32 /*inIOBufferFrameSize*/,
                                const AudioServerPlugInIOCycleInfo* /*inIOCycleInfo*/) {
    return kAudioHardwareNoError;
}

OSStatus DFOVC_DoIOOperation(AudioServerPlugInDriverRef /*inDriver*/,
                             AudioObjectID inDeviceObjectID,
                             AudioObjectID /*inStreamObjectID*/,
                             UInt32 /*inClientID*/,
                             UInt32 inOperationID,
                             UInt32 inIOBufferFrameSize,
                             const AudioServerPlugInIOCycleInfo* inIOCycleInfo,
                             void* ioMainBuffer,
                             void* /*ioSecondaryBuffer*/) {
    if (!inIOCycleInfo || !ioMainBuffer || inIOBufferFrameSize == 0) {
        return kAudioHardwareNoError;
    }

    uint32_t idx = 0;
    ObjectKind kind = ObjectKind::Invalid;
    if (!DecodeObjectID(inDeviceObjectID, idx, kind) || kind != ObjectKind::Device) {
        return kAudioHardwareBadObjectError;
    }

    CableState& c = gCables[idx];
    const uint64_t now = mach_absolute_time();
    const bool cableEnabled = c.enabled.load(std::memory_order_acquire);

    if (inOperationID == kAudioServerPlugInIOOperationWriteMix) {
        const int64_t sampleTime = static_cast<int64_t>(std::llround(inIOCycleInfo->mOutputTime.mSampleTime));
        const bool muted = !cableEnabled || c.outputMute.load(std::memory_order_acquire);
        const float gain = ScalarToLinearGain(c.outputVolumeScalar.load(std::memory_order_acquire));
        c.ring.Write(sampleTime,
                     static_cast<const float*>(ioMainBuffer),
                     inIOBufferFrameSize,
                     gain,
                     muted,
                     now);
    } else if (inOperationID == kAudioServerPlugInIOOperationReadInput) {
        const int64_t sampleTime = static_cast<int64_t>(std::llround(inIOCycleInfo->mInputTime.mSampleTime));
        const bool muted = !cableEnabled || c.inputMute.load(std::memory_order_acquire);
        const float gain = ScalarToLinearGain(c.inputVolumeScalar.load(std::memory_order_acquire));
        c.ring.Read(sampleTime,
                    static_cast<float*>(ioMainBuffer),
                    inIOBufferFrameSize,
                    gain,
                    muted,
                    now,
                    gActiveWindowTicks);
    }

    return kAudioHardwareNoError;
}

OSStatus DFOVC_EndIOOperation(AudioServerPlugInDriverRef /*inDriver*/,
                              AudioObjectID /*inDeviceObjectID*/,
                              UInt32 /*inClientID*/,
                              UInt32 /*inOperationID*/,
                              UInt32 /*inIOBufferFrameSize*/,
                              const AudioServerPlugInIOCycleInfo* /*inIOCycleInfo*/) {
    return kAudioHardwareNoError;
}

// ============================================================================
// Driver Interface VTable
// ============================================================================

AudioServerPlugInDriverInterface gDriverInterface = {
    nullptr, // _reserved
    DFOVC_QueryInterface,
    DFOVC_AddRef,
    DFOVC_Release,
    DFOVC_Initialize,
    DFOVC_CreateDevice,
    DFOVC_DestroyDevice,
    DFOVC_AddDeviceClient,
    DFOVC_RemoveDeviceClient,
    DFOVC_PerformDeviceConfigurationChange,
    DFOVC_AbortDeviceConfigurationChange,
    DFOVC_HasProperty,
    DFOVC_IsPropertySettable,
    DFOVC_GetPropertyDataSize,
    DFOVC_GetPropertyData,
    DFOVC_SetPropertyData,
    DFOVC_StartIO,
    DFOVC_StopIO,
    DFOVC_GetZeroTimeStamp,
    DFOVC_WillDoIOOperation,
    DFOVC_BeginIOOperation,
    DFOVC_DoIOOperation,
    DFOVC_EndIOOperation
};

} // namespace

// ============================================================================
// CFPlugIn Factory Entry Point
// ============================================================================

extern "C" __attribute__((visibility("default")))
void* DFOpenVCable_Create(CFAllocatorRef /*inAllocator*/, CFUUIDRef inRequestedTypeUUID) {
    if (inRequestedTypeUUID && CFEqual(inRequestedTypeUUID, kAudioServerPlugInTypeUUID)) {
        return &gDriverInterfacePtr;
    }
    return nullptr;
}
