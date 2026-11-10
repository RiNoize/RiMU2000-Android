// SPDX-License-Identifier: BSD-3-Clause
// Host test: g++ -std=c++20 -O2 -pthread midi_queue_test.cpp -o midi_queue_test
#include "../app/src/main/cpp/midi_queue.h"

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>
#include <utility>
#include <vector>

using rimu2000::MidiQueue;
using Entry = std::pair<std::uint8_t, std::uint8_t>;

static void capacity_and_wrap() {
    MidiQueue queue;
    const std::array<std::uint8_t, 3> note{0x90, 60, 100};
    assert(!queue.enqueue(note.data(), note.size(), 0));
    queue.setEnabled(true);

    // An oversized or overflowing SysEx submission must leave no partial bytes.
    std::vector<std::uint8_t> oversized(MidiQueue::capacity + 1, 0x55);
    assert(!queue.enqueue(oversized.data(), oversized.size(), 2));
    assert(queue.consume(MidiQueue::capacity, [](auto, auto) { assert(false); }) == 0);

    std::vector<std::uint8_t> fill(MidiQueue::capacity - 2);
    for (std::size_t i = 0; i < fill.size(); ++i) fill[i] = std::uint8_t(i);
    assert(queue.enqueue(fill.data(), fill.size(), 3));
    assert(!queue.enqueue(note.data(), note.size(), 0));
    std::size_t consumed = 0;
    assert(queue.consume(255, [&](auto byte, auto port) {
        assert(byte == fill[consumed++]);
        assert(port == 3);
    }) == 255);
    // This message crosses the end of the underlying circular storage.
    assert(queue.enqueue(note.data(), note.size(), 1));
    const auto tail = fill.size() - consumed;
    assert(queue.consume(tail, [&](auto byte, auto port) {
        assert(byte == fill[consumed++]);
        assert(port == 3);
    }) == tail);
    std::size_t noteBytes = 0;
    assert(queue.consume(10, [&](auto byte, auto port) {
        assert(byte == note[noteBytes++]);
        assert(port == 1);
    }) == note.size());
    assert(noteBytes == note.size());

    assert(queue.enqueue(note.data(), note.size(), 0));
    queue.discard();
    assert(queue.consume(10, [](auto, auto) { assert(false); }) == 0);
    queue.reset();
    assert(!queue.enqueue(note.data(), note.size(), 0));
    queue.setEnabled(true);
    assert(queue.enqueue(note.data(), note.size(), 2));
    noteBytes = 0;
    assert(queue.consume(10, [&](auto byte, auto port) {
        assert(byte == note[noteBytes++]);
        assert(port == 2);
    }) == note.size());
}

static std::vector<std::uint8_t> message(int producer, int sequence) {
    const auto length = std::size_t(10 + sequence % 17);
    std::vector<std::uint8_t> bytes(length);
    bytes[0] = 0xf0;
    bytes[1] = 0x7d; // noncommercial manufacturer ID for test data
    bytes[2] = std::uint8_t(producer);
    bytes[3] = std::uint8_t(length);
    for (int i = 0; i < 4; ++i) bytes[4 + i] = (sequence >> (i * 7)) & 0x7f;
    for (std::size_t i = 8; i + 1 < length; ++i)
        bytes[i] = std::uint8_t((sequence + producer * 31 + i) & 0x7f);
    bytes.back() = 0xf7;
    return bytes;
}

static void multiple_producers() {
    constexpr int producers = 4;
    constexpr int perProducer = 20000;
    MidiQueue queue;
    queue.setEnabled(true);
    std::atomic<bool> begin{false};
    std::vector<std::thread> threads;
    for (int producer = 0; producer < producers; ++producer) {
        threads.emplace_back([&, producer] {
            while (!begin.load(std::memory_order_acquire)) std::this_thread::yield();
            for (int sequence = 0; sequence < perProducer; ++sequence) {
                const auto bytes = message(producer, sequence);
                while (!queue.enqueue(bytes.data(), bytes.size(), std::uint8_t(producer)))
                    std::this_thread::yield();
            }
        });
    }
    begin.store(true, std::memory_order_release);
    std::array<int, producers> nextSequence{};
    std::vector<Entry> assembling;
    int complete = 0;
    std::size_t turn = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (complete < producers * perProducer) {
        // Consumer boundaries deliberately split messages at different offsets.
        const auto count = queue.consume(1 + (++turn % 97), [&](auto byte, auto port) {
            assembling.emplace_back(byte, port);
            if (assembling.size() == 1) assert(byte == 0xf0);
            if (assembling.size() < 4) return;
            const std::size_t length = assembling[3].first;
            assert(length >= 10 && length <= 26);
            if (assembling.size() < length) return;
            assert(assembling.size() == length);
            const int producer = assembling[2].first;
            assert(producer >= 0 && producer < producers);
            const auto expected = message(producer, nextSequence[producer]++);
            assert(expected.size() == length);
            for (std::size_t i = 0; i < length; ++i) {
                assert(assembling[i].first == expected[i]);
                assert(assembling[i].second == producer);
            }
            assembling.clear();
            ++complete;
        });
        if (!count) std::this_thread::yield();
        if (!(turn & 4095)) assert(std::chrono::steady_clock::now() < deadline);
    }
    for (auto& thread : threads) thread.join();
    assert(assembling.empty());
    for (auto count : nextSequence) assert(count == perProducer);
    assert(queue.consume(MidiQueue::capacity, [](auto, auto) { assert(false); }) == 0);
}

int main() {
    capacity_and_wrap();
    multiple_producers();
    std::cout << "MidiQueue: capacity, atomic rejection, wrap, reset/discard and "
                 "80,000 concurrent messages passed.\n";
}
