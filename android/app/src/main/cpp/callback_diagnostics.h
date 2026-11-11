#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace rimu2000 {

// Keep the shared payload atomic as well as its publication version. A version
// check alone would not make concurrent reads of ordinary C++ fields legal.
#define RIMU2000_CALLBACK_SAMPLE_FIELDS(X) \
    X(std::uint64_t, sequence, 0) \
    X(std::uint64_t, sinceStartNs, 0) \
    X(std::int32_t, frames, 0) \
    X(std::int32_t, rate, 0) \
    X(std::uint64_t, wallNs, 0) \
    X(std::uint64_t, cpuNs, 0) \
    X(std::uint64_t, coreNs, 0) \
    X(std::uint64_t, convertNs, 0) \
    X(std::uint64_t, startIntervalNs, 0) \
    X(std::int32_t, cpuStart, -1) \
    X(std::int32_t, cpuEnd, -1) \
    X(std::uint64_t, midiBytes, 0) \
    X(std::uint64_t, shCompileAttempts, 0) \
    X(std::uint64_t, shCompileFailures, 0) \
    X(std::uint64_t, masterBuildAttempts, 0) \
    X(std::uint64_t, masterBakeAttempts, 0) \
    X(std::uint64_t, slaveBuildAttempts, 0) \
    X(std::uint64_t, slaveBakeAttempts, 0) \
    X(std::int32_t, voicesMaster, 0) \
    X(std::int32_t, voicesSlave, 0) \
    X(std::int32_t, xrunDelta, 0) \
    X(std::int32_t, xrunTotal, 0) \
    X(bool, rusageValid, false) \
    X(std::int32_t, rusageError, 0) \
    X(std::uint64_t, minorFaults, 0) \
    X(std::uint64_t, majorFaults, 0) \
    X(std::uint64_t, involuntarySwitches, 0) \
    X(std::uint64_t, voluntarySwitches, 0) \
    X(std::uint64_t, patchWord, 0) \
    X(std::int32_t, patchPart, -1) \
    X(std::int32_t, midiRoute, -1)

// One callback writer; any number of non-realtime readers. record()/reset()
// must not overlap each other. The owner may also reset after closing audio.
// No allocations, locks, sorting, logging, or system calls occur here.
class CallbackDiagnostics {
public:
    static constexpr std::size_t kWorstCount = 8;
    static constexpr std::size_t kXrunCount = 4;

    struct Sample {
#define RIMU2000_DECLARE_FIELD(type, name, initial) type name = initial;
        RIMU2000_CALLBACK_SAMPLE_FIELDS(RIMU2000_DECLARE_FIELD)
#undef RIMU2000_DECLARE_FIELD
        bool operator==(const Sample&) const = default;
    };

    struct Snapshot {
        // Unsorted. A reader may sort its copy for display.
        std::array<Sample, kWorstCount> worst{};
        std::size_t worstCount = 0;
        // Oldest to newest, independently of inclusion among the worst eight.
        std::array<Sample, kXrunCount> xruns{};
        std::size_t xrunCount = 0;
        std::uint64_t generation = 0;
        bool operator==(const Snapshot&) const = default;
    };

    void record(const Sample& sample) noexcept {
        std::size_t worstIndex = kWorstCount;
        if (sample.frames > 0 && sample.rate > 0) {
            if (worstCount_ < kWorstCount) {
                worstIndex = worstCount_++;
            } else if (greaterLoad(sample, worst_[minimumIndex_])) {
                worstIndex = minimumIndex_;
            }
        }
        const bool addXrun = sample.xrunDelta > 0;
        if (worstIndex == kWorstCount && !addXrun) return;

        if (worstIndex != kWorstCount) {
            worst_[worstIndex] = sample;
            minimumIndex_ = 0;
            for (std::size_t i = 1; i < worstCount_; ++i) {
                if (greaterLoad(worst_[minimumIndex_], worst_[i])) minimumIndex_ = i;
            }
        }

        beginPublication();
        if (worstIndex != kWorstCount) {
            publishedWorst_[worstIndex].store(sample);
            publishedWorstCount_.store(std::uint32_t(worstCount_), std::memory_order_release);
        }
        if (addXrun) {
            publishedXruns_[xrunNext_].store(sample);
            xrunNext_ = (xrunNext_ + 1) % kXrunCount;
            if (xrunCount_ < kXrunCount) ++xrunCount_;
            publishedXrunNext_.store(std::uint32_t(xrunNext_), std::memory_order_release);
            publishedXrunCount_.store(std::uint32_t(xrunCount_), std::memory_order_release);
        }
        endPublication();
    }

    void reset() noexcept {
        // Stale slots need not be cleared: readers only copy the valid counts.
        worstCount_ = xrunCount_ = xrunNext_ = minimumIndex_ = 0;
        ++generation_;
        beginPublication();
        publishedWorstCount_.store(0, std::memory_order_release);
        publishedXrunCount_.store(0, std::memory_order_release);
        publishedXrunNext_.store(0, std::memory_order_release);
        publishedGeneration_.store(generation_, std::memory_order_release);
        endPublication();
    }

    // Bounded retries: UI can keep its last result if publication overlaps all
    // three attempts. A false result leaves the caller's destination untouched.
    bool read(Snapshot& output) const noexcept {
        for (int attempt = 0; attempt < 3; ++attempt) {
            const auto before = version_.load(std::memory_order_acquire);
            if (before & 1) continue;
            Snapshot result;
            result.generation = publishedGeneration_.load(std::memory_order_acquire);
            result.worstCount = publishedWorstCount_.load(std::memory_order_acquire);
            result.xrunCount = publishedXrunCount_.load(std::memory_order_acquire);
            const auto next = publishedXrunNext_.load(std::memory_order_acquire);
            for (std::size_t i = 0; i < result.worstCount; ++i)
                result.worst[i] = publishedWorst_[i].load();
            const auto first = result.xrunCount == kXrunCount ? next : 0;
            for (std::size_t i = 0; i < result.xrunCount; ++i)
                result.xruns[i] = publishedXruns_[(first + i) % kXrunCount].load();
            if (before == version_.load(std::memory_order_acquire)) {
                output = result;
                return true;
            }
        }
        return false;
    }

private:
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
    static_assert(std::atomic<std::int32_t>::is_always_lock_free);
    static_assert(std::atomic<bool>::is_always_lock_free);

    struct AtomicSample {
#define RIMU2000_ATOMIC_FIELD(type, name, initial) std::atomic<type> name{initial};
        RIMU2000_CALLBACK_SAMPLE_FIELDS(RIMU2000_ATOMIC_FIELD)
#undef RIMU2000_ATOMIC_FIELD

        void store(const Sample& sample) noexcept {
            // Release/acquire on the payload is deliberate: a reader seeing any
            // new field also sees the preceding odd version before its final
            // version check. Merely relaxed payload accesses would not provide
            // that ordering in the C++ memory model.
#define RIMU2000_STORE_FIELD(type, name, initial) name.store(sample.name, std::memory_order_release);
            RIMU2000_CALLBACK_SAMPLE_FIELDS(RIMU2000_STORE_FIELD)
#undef RIMU2000_STORE_FIELD
        }

        Sample load() const noexcept {
            Sample sample;
#define RIMU2000_LOAD_FIELD(type, name, initial) sample.name = name.load(std::memory_order_acquire);
            RIMU2000_CALLBACK_SAMPLE_FIELDS(RIMU2000_LOAD_FIELD)
#undef RIMU2000_LOAD_FIELD
            return sample;
        }
    };

    static bool greaterLoad(const Sample& a, const Sample& b) noexcept {
        // Compare wall/(frames/rate) exactly, without floating-point ties or
        // 64-bit product overflow. With positive int32 frames/rates the largest
        // cross-product is below 2^126. Android arm64 and the host
        // tests use Clang/GCC, which both provide this integer type.
        using Wide = unsigned __int128;
        const Wide left = Wide(a.wallNs) * std::uint32_t(a.rate) * std::uint32_t(b.frames);
        const Wide right = Wide(b.wallNs) * std::uint32_t(b.rate) * std::uint32_t(a.frames);
        return left > right;
    }

    void beginPublication() noexcept {
        version_.fetch_add(1, std::memory_order_relaxed);
    }
    void endPublication() noexcept {
        version_.fetch_add(1, std::memory_order_release);
    }

    // Ordinary fields below are exclusively owned by the callback writer.
    std::array<Sample, kWorstCount> worst_{};
    std::size_t worstCount_ = 0, minimumIndex_ = 0;
    std::size_t xrunCount_ = 0, xrunNext_ = 0;
    std::uint64_t generation_ = 0;

    std::array<AtomicSample, kWorstCount> publishedWorst_{};
    std::array<AtomicSample, kXrunCount> publishedXruns_{};
    std::atomic<std::uint32_t> publishedWorstCount_{0};
    std::atomic<std::uint32_t> publishedXrunCount_{0}, publishedXrunNext_{0};
    std::atomic<std::uint64_t> publishedGeneration_{0}, version_{0};
};

#undef RIMU2000_CALLBACK_SAMPLE_FIELDS

} // namespace rimu2000
