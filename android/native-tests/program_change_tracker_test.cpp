// SPDX-License-Identifier: BSD-3-Clause
#include "../app/src/main/cpp/program_change_tracker.h"

#include <cassert>
#include <cstdio>

int main() {
    rimu2000::ProgramChangeTracker tracker;
    assert(tracker.feed(12, 0) == -1);
    assert(tracker.feed(0xc2, 0) == -1); // status alone is not a change
    assert(tracker.feed(0xf8, 0) == -1); // interleaved clock
    assert(tracker.feed(0, 0) == 2);
    assert(tracker.feed(127, 0) == 2);   // running status
    assert(tracker.feed(0xcf, 3) == -1);
    assert(tracker.feed(5, 3) == 63);    // port state is independent
    assert(tracker.feed(4, 0) == 2);
    assert(tracker.feed(0x90, 0) == -1);
    assert(tracker.feed(60, 0) == -1);
    assert(tracker.feed(100, 0) == -1);
    assert(tracker.feed(0xc0, 0) == -1);
    assert(tracker.feed(0xf0, 0) == -1); // SysEx cancels running status
    assert(tracker.feed(0x43, 0) == -1);
    assert(tracker.feed(0xff, 0) == -1);
    assert(tracker.feed(0xf7, 0) == -1);
    assert(tracker.feed(5, 0) == -1);
    for (int system = 0xf1; system <= 0xf7; ++system) {
        tracker.feed(0xc1, 1);
        assert(tracker.feed(system, 1) == -1);
        assert(tracker.feed(3, 1) == -1);
    }
    assert(tracker.feed(0xc0, 9) == -1);
    assert(tracker.feed(0, 9) == -1);
    tracker.reset();
    assert(tracker.feed(5, 3) == -1);
    std::puts("Program Change tracking: fragmented, running status, ports and system bytes passed.");
}
