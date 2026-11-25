#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace dfovc {

// ============================================================================
// TimestampRingBuffer
// ----------------------------------------------------------------------------
// Realtime-safe, lock-free loopback ring buffer indexed directly by CoreAudio
// device sample time (int64_t frames).
//
// Why sample-time indexing?
// In a CoreAudio duplex AudioDevice, WriteMix receives frames scheduled at
// mOutputTime.mSampleTime (slightly in the future) and ReadInput reads frames
// at mInputTime.mSampleTime (slightly in the past) on the EXACT SAME device
// clock. Writing to ring[outSampleTime % capacity] and reading from
// ring[inSampleTime % capacity] guarantees:
//   - Zero clock drift
//   - Bit-exact, sample-accurate loopback
//   - Seamless handoff when a capture client joins an already-playing cable
//   - Zero locks or allocations on the CoreAudio realtime IO thread
// ============================================================================
class TimestampRingBuffer {
public:
    static constexpr int64_t kNoSampleTime = std::numeric_limits<int64_t>::min();

    TimestampRingBuffer() = default;

    // Must ONLY be called outside active IO (e.g. Initialize or PerformDeviceConfigurationChange).
    void Configure(uint32_t capacityFrames, uint32_t channels) {
        channels_ = channels > 0 ? channels : 2;
        // Minimum capacity 16384 frames so normal IO cycles never wrap prematurely.
        capacityFrames_ = std::max<uint32_t>(capacityFrames, 16384u);
        buffer_.assign(static_cast<size_t>(capacityFrames_) * channels_, 0.0f);
        ResetStreamState();
    }

    // Must ONLY be called outside active IO (e.g. StartIO 0->1 or StopIO 1->0).
    void ResetStreamState() {
        if (!buffer_.empty()) {
            std::memset(buffer_.data(), 0, buffer_.size() * sizeof(float));
        }
        writeStart_.store(kNoSampleTime, std::memory_order_relaxed);
        writeEnd_.store(kNoSampleTime, std::memory_order_relaxed);
        lastReadEnd_.store(kNoSampleTime, std::memory_order_relaxed);
        lastWriteHostTime_.store(0, std::memory_order_relaxed);
        lastReadHostTime_.store(0, std::memory_order_relaxed);
        currentFillFrames_.store(0, std::memory_order_relaxed);
        outputPeak_.store(0.0f, std::memory_order_relaxed);
        inputPeak_.store(0.0f, std::memory_order_relaxed);
    }

    void ResetStats() {
        overrunCount_.store(0, std::memory_order_relaxed);
        underrunCount_.store(0, std::memory_order_relaxed);
    }

    // Realtime-safe write called from kAudioServerPlugInIOOperationWriteMix.
    void Write(int64_t sampleTime,
               const float* src,
               uint32_t frameCount,
               float gain,
               bool mutedOrDisabled,
               uint64_t hostTimeNow) {
        if (frameCount == 0 || capacityFrames_ == 0 || buffer_.empty()) {
            return;
        }

        const int64_t cap = static_cast<int64_t>(capacityFrames_);
        const int64_t prevEnd = writeEnd_.load(std::memory_order_acquire);

        // If there is a forward gap since the previous write, zero-fill the gap
        // so stale audio from an earlier wrap can never leak into Read().
        if (prevEnd != kNoSampleTime && sampleTime > prevEnd) {
            const int64_t gap = sampleTime - prevEnd;
            if (gap >= cap) {
                std::memset(buffer_.data(), 0, buffer_.size() * sizeof(float));
            } else {
                ZeroRange(prevEnd, static_cast<uint32_t>(gap));
            }
        }

        // Check for overrun: if a reader is actively trailing and our new write
        // overlaps frames the reader hasn't consumed yet (distance > capacity).
        const int64_t readerEnd = lastReadEnd_.load(std::memory_order_acquire);
        const int64_t newEnd = sampleTime + static_cast<int64_t>(frameCount);
        if (readerEnd != kNoSampleTime && (newEnd - readerEnd) > cap) {
            overrunCount_.fetch_add(1, std::memory_order_relaxed);
        }

        // Handle rare case where a single write exceeds ring capacity.
        uint32_t framesToCopy = frameCount;
        int64_t dstSampleTime = sampleTime;
        const float* srcPtr = src;
        if (framesToCopy > capacityFrames_) {
            const uint32_t skip = framesToCopy - capacityFrames_;
            srcPtr += static_cast<size_t>(skip) * channels_;
            dstSampleTime += skip;
            framesToCopy = capacityFrames_;
            overrunCount_.fetch_add(1, std::memory_order_relaxed);
        }

        float peak = 0.0f;
        uint32_t startIdx = WrapIndex(dstSampleTime);
        uint32_t firstFrames = std::min(framesToCopy, capacityFrames_ - startIdx);
        uint32_t secondFrames = framesToCopy - firstFrames;

        if (mutedOrDisabled || srcPtr == nullptr) {
            std::memset(&buffer_[static_cast<size_t>(startIdx) * channels_],
                        0,
                        static_cast<size_t>(firstFrames) * channels_ * sizeof(float));
            if (secondFrames > 0) {
                std::memset(&buffer_[0],
                            0,
                            static_cast<size_t>(secondFrames) * channels_ * sizeof(float));
            }
        } else {
            CopyWithGainAndPeak(&buffer_[static_cast<size_t>(startIdx) * channels_],
                                srcPtr,
                                firstFrames * channels_,
                                gain,
                                peak);
            if (secondFrames > 0) {
                CopyWithGainAndPeak(&buffer_[0],
                                    srcPtr + static_cast<size_t>(firstFrames) * channels_,
                                    secondFrames * channels_,
                                    gain,
                                    peak);
            }
        }

        // Update valid written window [writeStart_, writeEnd_).
        int64_t newStart = writeStart_.load(std::memory_order_relaxed);
        if (newStart == kNoSampleTime || dstSampleTime < newStart) {
            newStart = dstSampleTime;
        }
        if (newEnd - newStart > cap) {
            newStart = newEnd - cap;
        }

        writeStart_.store(newStart, std::memory_order_relaxed);
        writeEnd_.store(newEnd, std::memory_order_release);
        lastWriteHostTime_.store(hostTimeNow, std::memory_order_release);
        outputPeak_.store(peak, std::memory_order_relaxed);

        // Update queue depth telemetry
        if (readerEnd != kNoSampleTime && newEnd > readerEnd) {
            const int64_t fill = std::min<int64_t>(newEnd - readerEnd, cap);
            currentFillFrames_.store(static_cast<uint32_t>(fill), std::memory_order_relaxed);
        } else {
            currentFillFrames_.store(framesToCopy, std::memory_order_relaxed);
        }
    }

    // Realtime-safe read called from kAudioServerPlugInIOOperationReadInput.
    void Read(int64_t sampleTime,
              float* dst,
              uint32_t frameCount,
              float gain,
              bool mutedOrDisabled,
              uint64_t hostTimeNow,
              uint64_t activeWindowTicks) {
        if (dst == nullptr || frameCount == 0) {
            return;
        }

        lastReadHostTime_.store(hostTimeNow, std::memory_order_release);
        const int64_t readEnd = sampleTime + static_cast<int64_t>(frameCount);
        lastReadEnd_.store(readEnd, std::memory_order_release);

        if (mutedOrDisabled || capacityFrames_ == 0 || buffer_.empty()) {
            std::memset(dst, 0, static_cast<size_t>(frameCount) * channels_ * sizeof(float));
            inputPeak_.store(0.0f, std::memory_order_relaxed);
            return;
        }

        const int64_t validEnd = writeEnd_.load(std::memory_order_acquire);
        const int64_t validStart = writeStart_.load(std::memory_order_acquire);
        const uint64_t lastWriteTicks = lastWriteHostTime_.load(std::memory_order_acquire);
        const bool writerRecentlyActive =
            (lastWriteTicks != 0) &&
            (hostTimeNow >= lastWriteTicks) &&
            ((hostTimeNow - lastWriteTicks) <= activeWindowTicks);

        // No playback writer has written yet or playback stopped -> clean digital silence.
        if (validEnd == kNoSampleTime || !writerRecentlyActive) {
            std::memset(dst, 0, static_cast<size_t>(frameCount) * channels_ * sizeof(float));
            inputPeak_.store(0.0f, std::memory_order_relaxed);
            currentFillFrames_.store(0, std::memory_order_relaxed);
            return;
        }

        const int64_t cap = static_cast<int64_t>(capacityFrames_);
        const int64_t earliestValid = std::max(validStart, validEnd - cap);

        // Determine overlap between requested [sampleTime, readEnd) and valid [earliestValid, validEnd).
        const int64_t copyStart = std::max(sampleTime, earliestValid);
        const int64_t copyEnd = std::min(readEnd, validEnd);

        if (copyStart >= copyEnd) {
            // Completely outside valid written window.
            std::memset(dst, 0, static_cast<size_t>(frameCount) * channels_ * sizeof(float));
            inputPeak_.store(0.0f, std::memory_order_relaxed);
            // Only count an underrun if the reader ran PAST an active writer's frontier.
            // Reading BEFORE earliestValid happens naturally during initial preroll cycles.
            if (sampleTime >= validEnd) {
                underrunCount_.fetch_add(1, std::memory_order_relaxed);
            }
            return;
        }

        // Zero any leading frames before earliestValid (preroll boundary)
        const uint32_t leadingZeros = static_cast<uint32_t>(copyStart - sampleTime);
        if (leadingZeros > 0) {
            std::memset(dst, 0, static_cast<size_t>(leadingZeros) * channels_ * sizeof(float));
        }

        // Copy valid overlapping frames from ring buffer
        const uint32_t copyFrames = static_cast<uint32_t>(copyEnd - copyStart);
        float* dstCopy = dst + static_cast<size_t>(leadingZeros) * channels_;
        uint32_t startIdx = WrapIndex(copyStart);
        uint32_t firstFrames = std::min(copyFrames, capacityFrames_ - startIdx);
        uint32_t secondFrames = copyFrames - firstFrames;

        float peak = 0.0f;
        CopyWithGainAndPeak(dstCopy,
                            &buffer_[static_cast<size_t>(startIdx) * channels_],
                            firstFrames * channels_,
                            gain,
                            peak);
        if (secondFrames > 0) {
            CopyWithGainAndPeak(dstCopy + static_cast<size_t>(firstFrames) * channels_,
                                &buffer_[0],
                                secondFrames * channels_,
                                gain,
                                peak);
        }

        // Zero any trailing frames past validEnd (real partial underrun)
        const uint32_t trailingZeros = static_cast<uint32_t>(readEnd - copyEnd);
        if (trailingZeros > 0) {
            std::memset(dstCopy + static_cast<size_t>(copyFrames) * channels_,
                        0,
                        static_cast<size_t>(trailingZeros) * channels_ * sizeof(float));
            underrunCount_.fetch_add(1, std::memory_order_relaxed);
        }

        inputPeak_.store(peak, std::memory_order_relaxed);

        if (validEnd > readEnd) {
            const int64_t fill = std::min<int64_t>(validEnd - readEnd, cap);
            currentFillFrames_.store(static_cast<uint32_t>(fill), std::memory_order_relaxed);
        } else {
            currentFillFrames_.store(0, std::memory_order_relaxed);
        }
    }

    uint32_t CapacityFrames() const { return capacityFrames_; }
    uint32_t CapacityBytes() const { return capacityFrames_ * channels_ * sizeof(float); }
    uint32_t CurrentFillBytes() const {
        return currentFillFrames_.load(std::memory_order_relaxed) * channels_ * sizeof(float);
    }
    uint64_t OverrunCount() const { return overrunCount_.load(std::memory_order_relaxed); }
    uint64_t UnderrunCount() const { return underrunCount_.load(std::memory_order_relaxed); }
    float OutputPeak() const { return outputPeak_.load(std::memory_order_relaxed); }
    float InputPeak() const { return inputPeak_.load(std::memory_order_relaxed); }

    bool IsWriterActive(uint64_t hostTimeNow, uint64_t activeWindowTicks) const {
        const uint64_t t = lastWriteHostTime_.load(std::memory_order_acquire);
        return t != 0 && hostTimeNow >= t && (hostTimeNow - t) <= activeWindowTicks;
    }

    bool IsReaderActive(uint64_t hostTimeNow, uint64_t activeWindowTicks) const {
        const uint64_t t = lastReadHostTime_.load(std::memory_order_acquire);
        return t != 0 && hostTimeNow >= t && (hostTimeNow - t) <= activeWindowTicks;
    }

private:
    uint32_t WrapIndex(int64_t sampleTime) const {
        const int64_t cap = static_cast<int64_t>(capacityFrames_);
        int64_t mod = sampleTime % cap;
        if (mod < 0) mod += cap;
        return static_cast<uint32_t>(mod);
    }

    void ZeroRange(int64_t startSampleTime, uint32_t frameCount) {
        uint32_t startIdx = WrapIndex(startSampleTime);
        uint32_t firstFrames = std::min(frameCount, capacityFrames_ - startIdx);
        uint32_t secondFrames = frameCount - firstFrames;
        std::memset(&buffer_[static_cast<size_t>(startIdx) * channels_],
                    0,
                    static_cast<size_t>(firstFrames) * channels_ * sizeof(float));
        if (secondFrames > 0) {
            std::memset(&buffer_[0],
                        0,
                        static_cast<size_t>(secondFrames) * channels_ * sizeof(float));
        }
    }

    static void CopyWithGainAndPeak(float* dst,
                                    const float* src,
                                    uint32_t sampleCount,
                                    float gain,
                                    float& ioPeak) {
        float localPeak = ioPeak;
        if (gain == 1.0f) {
            for (uint32_t i = 0; i < sampleCount; ++i) {
                const float v = src[i];
                dst[i] = v;
                const float a = std::fabs(v);
                if (a > localPeak) localPeak = a;
            }
        } else {
            for (uint32_t i = 0; i < sampleCount; ++i) {
                const float v = src[i] * gain;
                dst[i] = v;
                const float a = std::fabs(v);
                if (a > localPeak) localPeak = a;
            }
        }
        ioPeak = localPeak;
    }

    std::vector<float> buffer_;
    uint32_t capacityFrames_ = 0;
    uint32_t channels_ = 2;

    std::atomic<int64_t> writeStart_{kNoSampleTime};
    std::atomic<int64_t> writeEnd_{kNoSampleTime};
    std::atomic<int64_t> lastReadEnd_{kNoSampleTime};
    std::atomic<uint64_t> lastWriteHostTime_{0};
    std::atomic<uint64_t> lastReadHostTime_{0};
    std::atomic<uint32_t> currentFillFrames_{0};
    std::atomic<uint64_t> overrunCount_{0};
    std::atomic<uint64_t> underrunCount_{0};
    std::atomic<float> outputPeak_{0.0f};
    std::atomic<float> inputPeak_{0.0f};
};

} // namespace dfovc
