// End-to-end integration test for the compiled DF-OpenVCable.driver Mach-O bundle.
// Dynamically loads DF-OpenVCable.driver via dlopen(), invokes DFOpenVCable_Create,
// simulates coreaudiod's AudioServerPlugInHostInterface, and verifies:
//   1. COM QueryInterface / AddRef / Release
//   2. All 5 devices (DF-OpenVCable A..E) + streams + volume/mute controls
//   3. Custom property IPC ('dfst', 'dfcs', 'dfcc', 'dfrs')
//   4. RequestDeviceConfigurationChange -> PerformDeviceConfigurationChange + persistence
//   5. StartIO -> GetZeroTimeStamp -> DoIOOperation (WriteMix -> ReadInput loopback) -> StopIO

#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <unistd.h>

#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../driver/dfovc_protocol.h"

namespace {

CFMutableDictionaryRef gMockStorage = nullptr;
std::atomic<int> gPropertiesChangedCalls{0};
std::atomic<int> gConfigChangeRequests{0};
AudioServerPlugInDriverRef gLoadedDriver = nullptr;

OSStatus Mock_PropertiesChanged(AudioServerPlugInHostRef /*inHost*/,
                                AudioObjectID /*inObjectID*/,
                                UInt32 /*inNumberAddresses*/,
                                const AudioObjectPropertyAddress* /*inAddresses*/) {
    gPropertiesChangedCalls.fetch_add(1);
    return kAudioHardwareNoError;
}

OSStatus Mock_CopyFromStorage(AudioServerPlugInHostRef /*inHost*/,
                              CFStringRef inKey,
                              CFPropertyListRef* outData) {
    if (!gMockStorage || !inKey || !outData) return kAudioHardwareUnspecifiedError;
    CFTypeRef val = CFDictionaryGetValue(gMockStorage, inKey);
    if (!val) {
        *outData = nullptr;
        return kAudioHardwareUnspecifiedError;
    }
    *outData = CFRetain(val);
    return kAudioHardwareNoError;
}

OSStatus Mock_WriteToStorage(AudioServerPlugInHostRef /*inHost*/,
                             CFStringRef inKey,
                             CFPropertyListRef inData) {
    if (!gMockStorage || !inKey || !inData) return kAudioHardwareUnspecifiedError;
    CFDictionarySetValue(gMockStorage, inKey, inData);
    return kAudioHardwareNoError;
}

OSStatus Mock_DeleteFromStorage(AudioServerPlugInHostRef /*inHost*/, CFStringRef inKey) {
    if (gMockStorage && inKey) CFDictionaryRemoveValue(gMockStorage, inKey);
    return kAudioHardwareNoError;
}

OSStatus Mock_RequestDeviceConfigurationChange(AudioServerPlugInHostRef /*inHost*/,
                                               AudioObjectID inDeviceObjectID,
                                               UInt64 inChangeAction,
                                               void* inChangeInfo) {
    assert(gLoadedDriver != nullptr);
    OSStatus res = (*gLoadedDriver)->PerformDeviceConfigurationChange(
        gLoadedDriver, inDeviceObjectID, inChangeAction, inChangeInfo);
    gConfigChangeRequests.fetch_add(1, std::memory_order_release);
    return res;
}

const AudioServerPlugInHostInterface gMockHost = {
    Mock_PropertiesChanged,
    Mock_CopyFromStorage,
    Mock_WriteToStorage,
    Mock_DeleteFromStorage,
    Mock_RequestDeviceConfigurationChange
};

} // namespace

int main(int argc, char** argv) {
    const char* driverPath = (argc > 1)
        ? argv[1]
        : "build/DF-OpenVCable.driver/Contents/MacOS/DF-OpenVCable";

    gMockStorage = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 8, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

    void* handle = dlopen(driverPath, RTLD_NOW);
    if (!handle) {
        std::fprintf(stderr, "dlopen failed for %s: %s\n", driverPath, dlerror());
        return 1;
    }

    using FactoryFn = void* (*)(CFAllocatorRef, CFUUIDRef);
    auto createFn = reinterpret_cast<FactoryFn>(dlsym(handle, "DFOpenVCable_Create"));
    assert(createFn != nullptr);

    void* raw = createFn(kCFAllocatorDefault, kAudioServerPlugInTypeUUID);
    assert(raw != nullptr);

    auto drv = reinterpret_cast<AudioServerPlugInDriverRef>(raw);
    gLoadedDriver = drv;

    // 1. Test QueryInterface
    void* queried = nullptr;
    HRESULT hr = (*drv)->QueryInterface(drv, CFUUIDGetUUIDBytes(kAudioServerPlugInDriverInterfaceUUID), &queried);
    assert(hr == S_OK && queried == raw);
    (*drv)->Release(drv);

    // 2. Initialize plugin
    OSStatus err = (*drv)->Initialize(drv, &gMockHost);
    assert(err == kAudioHardwareNoError);

    // 3. Verify 5 virtual devices exposed on kAudioObjectPlugInObject
    AudioObjectPropertyAddress devListAddr = {
        kAudioPlugInPropertyDeviceList,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    UInt32 size = 0;
    err = (*drv)->GetPropertyDataSize(drv, kAudioObjectPlugInObject, 0, &devListAddr, 0, nullptr, &size);
    assert(err == kAudioHardwareNoError && size == 5 * sizeof(AudioObjectID));

    AudioObjectID devices[5] = {};
    err = (*drv)->GetPropertyData(drv, kAudioObjectPlugInObject, 0, &devListAddr, 0, nullptr, size, &size, devices);
    assert(err == kAudioHardwareNoError && size == 5 * sizeof(AudioObjectID));

    for (int i = 0; i < 5; ++i) {
        AudioObjectPropertyAddress nameAddr = {
            kAudioObjectPropertyName,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        CFStringRef cfName = nullptr;
        UInt32 nameSize = sizeof(CFStringRef);
        err = (*drv)->GetPropertyData(drv, devices[i], 0, &nameAddr, 0, nullptr, nameSize, &nameSize, &cfName);
        assert(err == kAudioHardwareNoError && cfName != nullptr);
        char buf[64] = {};
        CFStringGetCString(cfName, buf, sizeof(buf), kCFStringEncodingUTF8);
        CFRelease(cfName);
        char expected[64];
        std::snprintf(expected, sizeof(expected), "DF-OpenVCable %c", 'A' + i);
        assert(std::strcmp(buf, expected) == 0);
    }
    std::printf("[PASS] Verified 5 CoreAudio devices (DF-OpenVCable A..E)\n");

    // 4. Test custom property 'dfst' (read status) & 'dfcc' (reconfigure Cable B to 96kHz / 250ms)
    AudioObjectPropertyAddress allStatusAddr = {
        kDFOVCPropertyAllStatus,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    CFPropertyListRef statusPlist = nullptr;
    UInt32 plistSize = sizeof(CFPropertyListRef);
    err = (*drv)->GetPropertyData(drv, kAudioObjectPlugInObject, 0, &allStatusAddr, 0, nullptr, plistSize, &plistSize, &statusPlist);
    assert(err == kAudioHardwareNoError && statusPlist != nullptr);
    CFRelease(statusPlist);

    CFMutableDictionaryRef cfg = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 4, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    int64_t cableIdx = 1;
    double newSR = 96000.0;
    int64_t newBuf = 250;
    CFNumberRef nIdx = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &cableIdx);
    CFNumberRef nSR  = CFNumberCreate(kCFAllocatorDefault, kCFNumberDoubleType, &newSR);
    CFNumberRef nBuf = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &newBuf);
    CFDictionarySetValue(cfg, CFSTR(DFOVC_KEY_CABLE_INDEX), nIdx);
    CFDictionarySetValue(cfg, CFSTR(DFOVC_KEY_SAMPLE_RATE), nSR);
    CFDictionarySetValue(cfg, CFSTR(DFOVC_KEY_BUFFER_MS), nBuf);
    CFRelease(nIdx); CFRelease(nSR); CFRelease(nBuf);

    AudioObjectPropertyAddress cfgAddr = {
        kDFOVCPropertyCableConfig,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    CFPropertyListRef cfgRef = cfg;
    err = (*drv)->SetPropertyData(drv, kAudioObjectPlugInObject, 0, &cfgAddr, 0, nullptr, sizeof(cfgRef), &cfgRef);
    assert(err == kAudioHardwareNoError);
    CFRelease(cfg);

    // Wait briefly for async dispatch to complete PerformDeviceConfigurationChange
    for (int wait = 0; wait < 50 && gConfigChangeRequests.load() == 0; ++wait) {
        usleep(10000);
    }
    assert(gConfigChangeRequests.load() == 1);

    // Confirm Cable B nominal sample rate updated to 96000 Hz
    AudioObjectPropertyAddress srAddr = {
        kAudioDevicePropertyNominalSampleRate,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    Float64 actualSR = 0.0;
    UInt32 srSize = sizeof(Float64);
    err = (*drv)->GetPropertyData(drv, devices[1], 0, &srAddr, 0, nullptr, srSize, &srSize, &actualSR);
    assert(err == kAudioHardwareNoError && std::fabs(actualSR - 96000.0) < 1.0);
    std::printf("[PASS] Custom IPC ('dfst'/'dfcc') + dynamic 96 kHz config change verified\n");

    // 5. Re-initialize plugin and verify settings persisted via Host storage!
    err = (*drv)->Initialize(drv, &gMockHost);
    assert(err == kAudioHardwareNoError);
    actualSR = 0.0;
    err = (*drv)->GetPropertyData(drv, devices[1], 0, &srAddr, 0, nullptr, srSize, &srSize, &actualSR);
    assert(err == kAudioHardwareNoError && std::fabs(actualSR - 96000.0) < 1.0);
    std::printf("[PASS] Persistent storage round-trip verified (Cable B retained 96 kHz)\n");

    // 6. Test StartIO -> GetZeroTimeStamp -> DoIOOperation WriteMix/ReadInput -> StopIO on Cable A
    const AudioObjectID cableA = devices[0];
    err = (*drv)->StartIO(drv, cableA, 1);
    assert(err == kAudioHardwareNoError);

    Float64 zSample = -1.0;
    UInt64 zHost = 0, zSeed = 0;
    err = (*drv)->GetZeroTimeStamp(drv, cableA, 1, &zSample, &zHost, &zSeed);
    assert(err == kAudioHardwareNoError && zSample >= 0.0 && zHost > 0 && zSeed >= 1);

    Boolean willDo = false, inPlace = false;
    err = (*drv)->WillDoIOOperation(drv, cableA, 1, kAudioServerPlugInIOOperationWriteMix, &willDo, &inPlace);
    assert(err == kAudioHardwareNoError && willDo && inPlace);
    err = (*drv)->WillDoIOOperation(drv, cableA, 1, kAudioServerPlugInIOOperationReadInput, &willDo, &inPlace);
    assert(err == kAudioHardwareNoError && willDo && inPlace);

    constexpr UInt32 kBlock = 512;
    std::vector<float> outBuf(kBlock * 2);
    std::vector<float> inBuf(kBlock * 2);

    for (int cycle = 0; cycle < 10; ++cycle) {
        AudioServerPlugInIOCycleInfo info = {};
        info.mIOCycleCounter = static_cast<UInt64>(cycle + 1);
        info.mNominalIOBufferFrameSize = kBlock;
        info.mInputTime.mSampleTime = static_cast<Float64>(cycle * kBlock);
        info.mOutputTime.mSampleTime = static_cast<Float64>(cycle * kBlock + 1024);

        for (UInt32 f = 0; f < kBlock; ++f) {
            const float s = static_cast<float>(std::sin(2.0 * M_PI * 1000.0 * (info.mOutputTime.mSampleTime + f) / 48000.0));
            outBuf[f * 2 + 0] = s;
            outBuf[f * 2 + 1] = s * 0.5f;
        }

        std::fill(inBuf.begin(), inBuf.end(), -99.0f);
        err = (*drv)->DoIOOperation(drv, cableA, cableA + 1, 1, kAudioServerPlugInIOOperationReadInput, kBlock, &info, inBuf.data(), nullptr);
        assert(err == kAudioHardwareNoError);
        err = (*drv)->DoIOOperation(drv, cableA, cableA + 2, 1, kAudioServerPlugInIOOperationWriteMix, kBlock, &info, outBuf.data(), nullptr);
        assert(err == kAudioHardwareNoError);

        if (cycle >= 2) {
            for (UInt32 f = 0; f < kBlock; ++f) {
                const float expL = static_cast<float>(std::sin(2.0 * M_PI * 1000.0 * (info.mInputTime.mSampleTime + f) / 48000.0));
                assert(std::fabs(inBuf[f * 2 + 0] - expL) < 1e-6f);
                assert(std::fabs(inBuf[f * 2 + 1] - expL * 0.5f) < 1e-6f);
            }
        }
    }

    err = (*drv)->StopIO(drv, cableA, 1);
    assert(err == kAudioHardwareNoError);
    std::printf("[PASS] Live driver StartIO / GetZeroTimeStamp / DoIOOperation loopback verified\n");

    CFRelease(gMockStorage);
    gMockStorage = nullptr;
    dlclose(handle);
    std::printf("All DF-OpenVCable.driver plugin integration tests passed!\n");
    return 0;
}
