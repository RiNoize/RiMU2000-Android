#include "../app/src/main/cpp/callback_diagnostics.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <limits>
#include <thread>
#include <vector>

using Diagnostics = rimu2000::CallbackDiagnostics;
using Sample = Diagnostics::Sample;
using Snapshot = Diagnostics::Snapshot;

namespace {

void check(bool condition, const char* description) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", description);
        std::abort();
    }
}

Snapshot snapshot(const Diagnostics& diagnostics) {
    Snapshot result;
    check(diagnostics.read(result), "read succeeds without a concurrent writer");
    return result;
}

Sample sample(std::uint64_t sequence, std::uint64_t wallNs,
              std::int32_t frames = 480, std::int32_t rate = 48000) {
    Sample result;
    result.sequence = sequence;
    result.frames = frames;
    result.rate = rate;
    result.wallNs = wallNs;
    return result;
}

std::vector<std::uint64_t> worstSequences(const Snapshot& value) {
    std::vector<std::uint64_t> result;
    for (std::size_t i = 0; i < value.worstCount; ++i)
        result.push_back(value.worst[i].sequence);
    std::sort(result.begin(), result.end());
    return result;
}

void expectWorst(const Snapshot& value, std::initializer_list<std::uint64_t> expected) {
    check(worstSequences(value) == std::vector<std::uint64_t>(expected),
          "retained callback identities match the expected work ratios");
}

void testInitialAndPayload() {
    Diagnostics diagnostics;
    check(snapshot(diagnostics) == Snapshot{}, "initial snapshot is empty");
    Sample rich = sample(123, 5000000);
    rich.sinceStartNs = 123456789;
    rich.cpuNs = 4000000;
    rich.coreNs = 3800000;
    rich.convertNs = 200000;
    rich.startIntervalNs = 10000000;
    rich.cpuStart = 6;
    rich.cpuEnd = 7;
    rich.midiBytes = 24;
    rich.shCompileAttempts = 5;
    rich.shCompileFailures = 1;
    rich.masterBuildAttempts = 2;
    rich.masterBakeAttempts = 3;
    rich.slaveBuildAttempts = 4;
    rich.slaveBakeAttempts = 6;
    rich.voicesMaster = 64;
    rich.voicesSlave = 37;
    rich.xrunDelta = 2;
    rich.xrunTotal = 9;
    rich.rusageValid = true;
    rich.minorFaults = 11;
    rich.majorFaults = 12;
    rich.involuntarySwitches = 13;
    rich.voluntarySwitches = 14;
    rich.patchWord = 0x0123456789abcdefULL;
    rich.patchPart = 31;
    rich.midiRoute = 63;
    diagnostics.record(rich);
    const auto first = snapshot(diagnostics);
    check(first.worstCount == 1 && first.xrunCount == 1, "first record enters both collections");
    check(first.worst[0] == rich && first.xruns[0] == rich, "all diagnostic fields survive publication");
    rich.sequence = 124;
    rich.rusageValid = false;
    rich.rusageError = 22;
    diagnostics.record(rich);
    const auto second = snapshot(diagnostics);
    check(second.worst[1] == rich && second.xruns[1] == rich, "failed rusage remains distinct from valid measurements");
    check(first.worst[0].sequence == 123, "reader copies remain independent of subsequent records");
}

void testTopEight() {
    Diagnostics diagnostics;
    for (std::uint64_t i = 1; i <= 20; ++i) diagnostics.record(sample(i, i * 100000));
    expectWorst(snapshot(diagnostics), {13, 14, 15, 16, 17, 18, 19, 20});
    const auto retained = snapshot(diagnostics);
    diagnostics.record(sample(21, 1));
    check(snapshot(diagnostics) == retained, "a lower-cost callback changes no published record");
}

void testNormalizedDurationsAndTies() {
    Diagnostics diagnostics;
    // Baseline: 10%, 20%, ..., 80% of ten milliseconds of output audio.
    for (std::uint64_t i = 1; i <= 8; ++i) diagnostics.record(sample(i, i * 1000000));
    diagnostics.record(sample(9, 50000000, 4800));       // 50%, despite much longer wall time.
    diagnostics.record(sample(10, 900000, 48));         // 90%, despite much shorter wall time.
    diagnostics.record(sample(11, 4750000, 480, 96000)); // 95% at the higher sample rate.
    diagnostics.record(sample(12, 10000000, 480, 24000));// 50% at the lower sample rate.
    expectWorst(snapshot(diagnostics), {5, 6, 7, 8, 9, 10, 11, 12});
    const auto retained = snapshot(diagnostics);
    diagnostics.record(sample(13, 1250000, 120));        // Exactly tied with the retained minimum.
    check(snapshot(diagnostics) == retained, "exact ties keep older records without republishing");
}

void testWideIntegerComparison() {
    Diagnostics diagnostics;
    constexpr auto maxWall = std::numeric_limits<std::uint64_t>::max();
    constexpr auto maxCount = std::numeric_limits<std::int32_t>::max();
    // These neighboring durations collapse to the same double. Their exact
    // cross-products also overflow 64 bits, so compare by known integer order.
    for (std::uint64_t i = 1; i <= 8; ++i)
        diagnostics.record(sample(i, maxWall - 9 + i, maxCount, maxCount));
    diagnostics.record(sample(9, maxWall, maxCount, maxCount));
    expectWorst(snapshot(diagnostics), {2, 3, 4, 5, 6, 7, 8, 9});
    diagnostics.record(sample(10, maxWall, maxCount, 1));
    expectWorst(snapshot(diagnostics), {2, 3, 4, 5, 6, 7, 8, 9});
}

void testXrunRetention() {
    Diagnostics diagnostics;
    for (std::uint64_t i = 1; i <= 12; ++i) {
        auto value = sample(i, 1000000 - i * 10000);
        value.xrunDelta = (i % 2) == 0 ? 1 : 0;
        value.xrunTotal = std::int32_t(i / 2);
        diagnostics.record(value);
    }
    const auto retained = snapshot(diagnostics);
    expectWorst(retained, {1, 2, 3, 4, 5, 6, 7, 8});
    check(retained.xrunCount == 4, "four recent xrun observations are retained");
    for (std::size_t i = 0; i < 4; ++i)
        check(retained.xruns[i].sequence == 6 + i * 2, "xrun ring reads chronologically across wrap");
    auto noIncrement = sample(13, 1);
    noIncrement.xrunTotal = 6;
    diagnostics.record(noIncrement);
    check(snapshot(diagnostics) == retained, "cumulative xrun count alone does not add an incident");
}

void testInvalidDimensionsAndReset() {
    Diagnostics diagnostics;
    diagnostics.record(sample(1, 100, 0));
    diagnostics.record(sample(2, 100, -1));
    diagnostics.record(sample(3, 100, 480, 0));
    diagnostics.record(sample(4, 100, 480, -1));
    check(snapshot(diagnostics) == Snapshot{}, "invalid audio durations cannot enter normalized ranking");
    auto invalidWithXrun = sample(5, 100, 0);
    invalidWithXrun.xrunDelta = 1;
    diagnostics.record(invalidWithXrun);
    auto retained = snapshot(diagnostics);
    check(retained.worstCount == 0 && retained.xrunCount == 1,
          "xrun evidence is retained even without a valid normalization duration");
    check(retained.xruns[0] == invalidWithXrun, "invalid duration is not silently rewritten");
    auto zeroTime = sample(6, 0);
    zeroTime.xrunDelta = -1;
    diagnostics.record(zeroTime);
    retained = snapshot(diagnostics);
    check(retained.worstCount == 1 && retained.worst[0] == zeroTime,
          "zero measured wall time is a valid nonnegative workload");
    check(retained.xrunCount == 1, "negative xrun error is not counted as an incident");
    diagnostics.reset();
    Snapshot empty;
    empty.generation = 1;
    check(snapshot(diagnostics) == empty, "reset publishes a new empty generation with no stale payload");
    auto fresh = sample(7, 200);
    fresh.xrunDelta = 1;
    diagnostics.record(fresh);
    retained = snapshot(diagnostics);
    check(retained.generation == 1 && retained.worstCount == 1 && retained.xrunCount == 1,
          "retention restarts normally after reset");
    check(retained.worst[0] == fresh && retained.xruns[0] == fresh, "prior generation does not reappear");
    diagnostics.reset();
    empty.generation = 2;
    check(snapshot(diagnostics) == empty, "repeated reset advances generation");
}

Sample taggedSample(std::uint64_t sequence) {
    auto value = sample(sequence, 100000 + (sequence % 12345) * 187,
                        std::int32_t(128 + (sequence % 3) * 256),
                        (sequence & 1) ? 48000 : 96000);
    value.sinceStartNs = sequence * 1000000;
    value.cpuNs = sequence * 101 + 1;
    value.coreNs = sequence * 102 + 2;
    value.convertNs = sequence * 103 + 3;
    value.startIntervalNs = sequence * 104 + 4;
    value.cpuStart = std::int32_t(sequence % 8);
    value.cpuEnd = std::int32_t((sequence + 1) % 8);
    value.midiBytes = sequence * 105 + 5;
    value.shCompileAttempts = sequence * 106 + 6;
    value.shCompileFailures = sequence * 107 + 7;
    value.masterBuildAttempts = sequence * 108 + 8;
    value.masterBakeAttempts = sequence * 109 + 9;
    value.slaveBuildAttempts = sequence * 110 + 10;
    value.slaveBakeAttempts = sequence * 111 + 11;
    value.voicesMaster = std::int32_t(sequence % 65);
    value.voicesSlave = std::int32_t((sequence + 7) % 65);
    value.xrunDelta = 1;
    value.xrunTotal = std::int32_t(sequence);
    value.rusageValid = (sequence & 1) != 0;
    value.rusageError = value.rusageValid ? 0 : 22;
    value.minorFaults = sequence * 112 + 12;
    value.majorFaults = sequence * 113 + 13;
    value.involuntarySwitches = sequence * 114 + 14;
    value.voluntarySwitches = sequence * 115 + 15;
    value.patchWord = sequence ^ 0xabcdf01234567890ULL;
    value.patchPart = std::int32_t(sequence % 64);
    value.midiRoute = std::int32_t((sequence + 9) % 64);
    return value;
}

bool coherent(const Snapshot& value) {
    if (value.worstCount > Diagnostics::kWorstCount || value.xrunCount > Diagnostics::kXrunCount)
        return false;
    const auto validSample = [&](const Sample& item) {
        return item.sequence / 1000000 == value.generation && item == taggedSample(item.sequence);
    };
    for (std::size_t i = 0; i < value.worstCount; ++i)
        if (!validSample(value.worst[i])) return false;
    for (std::size_t i = 0; i < value.xrunCount; ++i) {
        if (!validSample(value.xruns[i])) return false;
        if (i && value.xruns[i - 1].sequence >= value.xruns[i].sequence) return false;
    }
    return true;
}

void testConcurrentReadersAndReset() {
    Diagnostics diagnostics;
    std::atomic<int> ready{0};
    std::atomic<bool> go{false}, done{false};
    std::atomic<std::uint64_t> successfulReads{0}, overlappingReads{0};
    std::array<std::atomic<std::uint64_t>, 2> acknowledged{};
    const auto reader = [&](int readerIndex) {
        ready.fetch_add(1, std::memory_order_release);
        while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
        Snapshot output;
        do {
            const Snapshot previous = output;
            if (diagnostics.read(output)) {
                check(coherent(output), "concurrent snapshot never mixes fields or reset generations");
                successfulReads.fetch_add(1, std::memory_order_relaxed);
                if (output.xrunCount)
                    acknowledged[readerIndex].store(output.xruns[output.xrunCount - 1].sequence,
                                                     std::memory_order_release);
            } else {
                check(output == previous, "failed bounded read leaves the destination untouched");
                overlappingReads.fetch_add(1, std::memory_order_relaxed);
            }
        } while (!done.load(std::memory_order_acquire));
        check(diagnostics.read(output) && coherent(output), "final reader snapshot is coherent");
    };
    std::thread first(reader, 0), second(reader, 1);
    while (ready.load(std::memory_order_acquire) != 2) std::this_thread::yield();
    go.store(true, std::memory_order_release);
    for (std::uint64_t generation = 1; generation <= 60; ++generation) {
        diagnostics.reset();
        for (std::uint64_t i = 1; i <= 400; ++i) {
            const auto sequence = generation * 1000000 + i;
            diagnostics.record(taggedSample(sequence));
            // Continuous writes exercise rejection of overlapping snapshots.
            // Periodic gaps also require both readers to accept actual payloads
            // from every generation, rather than passing by rejecting all work.
            if ((i % 100) == 0) {
                while (acknowledged[0].load(std::memory_order_acquire) < sequence ||
                       acknowledged[1].load(std::memory_order_acquire) < sequence)
                    std::this_thread::yield();
            }
        }
    }
    done.store(true, std::memory_order_release);
    first.join();
    second.join();
    const auto result = snapshot(diagnostics);
    check(result.generation == 60 && result.worstCount == 8 && result.xrunCount == 4,
          "final generation retains expected collection sizes");
    for (std::size_t i = 0; i < 4; ++i)
        check(result.xruns[i].sequence == 60000397 + i, "concurrent xrun ring ends at the actual newest records");
    check(successfulReads.load() >= 480, "both readers accepted actual payloads throughout all generations");
    std::printf("Concurrent read attempts: %llu coherent, %llu deferred.\n",
                static_cast<unsigned long long>(successfulReads.load()),
                static_cast<unsigned long long>(overlappingReads.load()));
}

} // namespace

int main() {
    testInitialAndPayload();
    testTopEight();
    testNormalizedDurationsAndTies();
    testWideIntegerComparison();
    testXrunRetention();
    testInvalidDimensionsAndReset();
    testConcurrentReadersAndReset();
    std::puts("Callback diagnostics: 7 test groups passed.");
}
