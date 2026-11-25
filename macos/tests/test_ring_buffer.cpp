// Standalone unit tests for DF-OpenVCable macOS TimestampRingBuffer.
// Verifies bit-exact loopback, wrap-around, gap zero-filling, preroll vs underrun,
// overrun detection, mute/disable silencing, gain scaling, and 1 kHz sine Goertzel integrity.

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "../driver/ring_buffer.h"

static constexpr double kPi = 3.14159265358979323846;

static double GoertzelMagnitude(const std::vector<float>& mono, double sampleRate, double targetFreq) {
    const size_t N = mono.size();
    const double k = std::round((static_cast<double>(N) * targetFreq) / sampleRate);
    const double omega = (2.0 * kPi * k) / static_cast<double>(N);
    const double coeff = 2.0 * std::cos(omega);
    double s0 = 0.0, s1 = 0.0, s2 = 0.0;
    for (float x : mono) {
        s0 = static_cast<double>(x) + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
    return (2.0 * std::sqrt(std::max(0.0, power))) / static_cast<double>(N);
}

static void TestBitExactLoopbackAndWrapAround() {
    dfovc::TimestampRingBuffer ring;
    ring.Configure(16384, 2);

    const uint32_t block = 512;
    const uint64_t activeWin = 100000;
    uint64_t t = 1000;

    // Simulate 100 IO cycles (51,200 frames -> wraps > 3x around 16,384 ring)
    // Output is scheduled 1024 frames ahead of Input, just like CoreAudio HAL.
    for (int cycle = 0; cycle < 100; ++cycle) {
        const int64_t inTime = static_cast<int64_t>(cycle) * block;
        const int64_t outTime = inTime + 1024;

        std::vector<float> outBuf(block * 2);
        for (uint32_t f = 0; f < block; ++f) {
            const float v = std::sin(2.0 * kPi * 1000.0 * static_cast<double>(outTime + f) / 48000.0);
            outBuf[f * 2 + 0] = v;
            outBuf[f * 2 + 1] = -v;
        }

        std::vector<float> inBuf(block * 2, 99.0f);
        ring.Read(inTime, inBuf.data(), block, 1.0f, false, t, activeWin);
        ring.Write(outTime, outBuf.data(), block, 1.0f, false, t);
        t += 100;

        if (cycle < 2) {
            // Initial preroll cycles must output clean silence and NOT count underruns
            for (float s : inBuf) assert(s == 0.0f);
            assert(ring.UnderrunCount() == 0);
        } else {
            // From cycle 2 onward, Read(inTime) must match exact samples written 2 cycles ago
            for (uint32_t f = 0; f < block; ++f) {
                const float expected = std::sin(2.0 * kPi * 1000.0 * static_cast<double>(inTime + f) / 48000.0);
                assert(std::fabs(inBuf[f * 2 + 0] - expected) < 1e-6f);
                assert(std::fabs(inBuf[f * 2 + 1] - (-expected)) < 1e-6f);
            }
        }
    }

    assert(ring.OverrunCount() == 0);
    assert(ring.UnderrunCount() == 0);
    std::printf("[PASS] TestBitExactLoopbackAndWrapAround\n");
}

static void TestSilenceWhenIdleAndMute() {
    dfovc::TimestampRingBuffer ring;
    ring.Configure(16384, 2);

    std::vector<float> outBuf(512 * 2, 0.75f);
    std::vector<float> inBuf(512 * 2, 1.0f);

    // Write at t=1000, read much later (past activeWindow) -> clean silence, 0 underruns
    ring.Write(1024, outBuf.data(), 512, 1.0f, false, 1000);
    ring.Read(1024, inBuf.data(), 512, 1.0f, false, 500000, 10000);
    for (float s : inBuf) assert(s == 0.0f);
    assert(ring.UnderrunCount() == 0);

    // Muted write -> silence even when read immediately
    ring.Write(2048, outBuf.data(), 512, 1.0f, true, 500100);
    ring.Read(2048, inBuf.data(), 512, 1.0f, false, 500200, 10000);
    for (float s : inBuf) assert(s == 0.0f);

    // Unmuted write, muted read -> silence
    ring.Write(3072, outBuf.data(), 512, 1.0f, false, 500300);
    ring.Read(3072, inBuf.data(), 512, 1.0f, true, 500400, 10000);
    for (float s : inBuf) assert(s == 0.0f);

    std::printf("[PASS] TestSilenceWhenIdleAndMute\n");
}

static void TestGapZeroFillAndGlitchCounters() {
    dfovc::TimestampRingBuffer ring;
    ring.Configure(16384, 2);

    std::vector<float> ones(512 * 2, 0.5f);
    std::vector<float> inBuf(512 * 2, 0.0f);

    // Write [0..512), then jump to [1536..2048) leaving gap [512..1536)
    ring.Write(0, ones.data(), 512, 1.0f, false, 1000);
    ring.Write(1536, ones.data(), 512, 1.0f, false, 1100);

    // Reading the gap [512..1024) must yield 0.0f (no stale data)
    ring.Read(512, inBuf.data(), 512, 1.0f, false, 1200, 10000);
    for (float s : inBuf) assert(s == 0.0f);

    // Reading beyond writeEnd while writer is active triggers underrun
    ring.Read(2048, inBuf.data(), 512, 1.0f, false, 1300, 10000);
    assert(ring.UnderrunCount() == 1);

    // Writer jumping > capacity ahead of reader triggers overrun
    ring.Write(30000, ones.data(), 512, 1.0f, false, 1400);
    assert(ring.OverrunCount() >= 1);

    ring.ResetStats();
    assert(ring.OverrunCount() == 0);
    assert(ring.UnderrunCount() == 0);

    std::printf("[PASS] TestGapZeroFillAndGlitchCounters\n");
}

static void Test1kHzSineGoertzelAndGain() {
    dfovc::TimestampRingBuffer ring;
    ring.Configure(24000, 2); // 500 ms at 48 kHz

    const uint32_t block = 256;
    const int cycles = 40;
    std::vector<float> capturedLeft;
    capturedLeft.reserve(block * cycles);

    uint64_t t = 1000;
    for (int c = 0; c < cycles; ++c) {
        const int64_t inTime = static_cast<int64_t>(c) * block;
        const int64_t outTime = inTime + 512;

        std::vector<float> outBuf(block * 2);
        for (uint32_t f = 0; f < block; ++f) {
            const float v = 0.8f * static_cast<float>(std::sin(2.0 * kPi * 1000.0 * static_cast<double>(outTime + f) / 48000.0));
            outBuf[f * 2 + 0] = v;
            outBuf[f * 2 + 1] = v;
        }
        ring.Write(outTime, outBuf.data(), block, 0.5f, false, t);

        std::vector<float> inBuf(block * 2, 0.0f);
        ring.Read(inTime, inBuf.data(), block, 1.0f, false, t + 10, 10000);
        if (c >= 4) {
            for (uint32_t f = 0; f < block; ++f) {
                capturedLeft.push_back(inBuf[f * 2 + 0]);
            }
        }
        t += 100;
    }

    // 0.8 amplitude * 0.5 gain = 0.4 expected 1 kHz tone magnitude
    const double mag1k = GoertzelMagnitude(capturedLeft, 48000.0, 1000.0);
    const double mag500 = GoertzelMagnitude(capturedLeft, 48000.0, 500.0);
    assert(std::fabs(mag1k - 0.4) < 0.02);
    assert(mag500 < 0.01);

    std::printf("[PASS] Test1kHzSineGoertzelAndGain (1kHz=%.4f, 500Hz=%.4f)\n", mag1k, mag500);
}

int main() {
    TestBitExactLoopbackAndWrapAround();
    TestSilenceWhenIdleAndMute();
    TestGapZeroFillAndGlitchCounters();
    Test1kHzSineGoertzelAndGain();
    std::printf("All TimestampRingBuffer unit tests passed!\n");
    return 0;
}
