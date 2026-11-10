// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace rimu2000 {

// Java's MIDI receiver and touch controls are separate producers. They may
// wait for one another, but the audio consumer never locks or allocates.
// A submission is published in full, or rejected in full (including SysEx).
class MidiQueue {
public:
    static constexpr std::size_t capacity = 65536;

    bool enqueue(const std::uint8_t* bytes, std::size_t count, std::uint8_t port) {
        std::lock_guard<std::mutex> guard(producers_);
        if (!enabled_.load(std::memory_order_relaxed)) return false;
        const auto w = write_.load(std::memory_order_relaxed);
        const auto r = read_.load(std::memory_order_acquire);
        if (count > capacity || count > capacity - (w - r)) return false;
        for (std::size_t i = 0; i < count; ++i)
            bytes_[(w + i) & (capacity - 1)] =
                std::uint16_t(bytes[i]) | (std::uint16_t(port & 3) << 8);
        write_.store(w + count, std::memory_order_release);
        return true;
    }

    template<class F>
    std::size_t consume(std::size_t limit, F&& receive) {
        auto r = read_.load(std::memory_order_relaxed);
        const auto w = write_.load(std::memory_order_acquire);
        const std::size_t count = std::size_t(w - r) < limit ? std::size_t(w - r) : limit;
        for (std::size_t i = 0; i < count; ++i) {
            const auto entry = bytes_[r++ & (capacity - 1)];
            receive(std::uint8_t(entry), std::uint8_t(entry >> 8));
        }
        read_.store(r, std::memory_order_release);
        return count;
    }

    // Audio consumer only, e.g. panic: drop messages preceding this point.
    void discard() {
        read_.store(write_.load(std::memory_order_acquire), std::memory_order_release);
    }

    void setEnabled(bool enabled) { enabled_.store(enabled, std::memory_order_release); }

    // Only call after closing the audio stream. Serializes with all producers.
    void reset() {
        std::lock_guard<std::mutex> guard(producers_);
        enabled_.store(false, std::memory_order_release);
        write_.store(0, std::memory_order_relaxed);
        read_.store(0, std::memory_order_relaxed);
    }

private:
    std::array<std::uint16_t, capacity> bytes_{};
    alignas(64) std::atomic<std::uint64_t> write_{0};
    alignas(64) std::atomic<std::uint64_t> read_{0};
    std::atomic<bool> enabled_{false};
    std::mutex producers_;
};

} // namespace rimu2000
