// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include <array>
#include <cstdint>

namespace rimu2000 {

// Observe complete Program Change messages without altering the MIDI stream.
// PC has one data byte. Other channel messages need not be assembled to track
// its running status. System Common cancels it; Real Time does not.
class ProgramChangeTracker {
public:
    void reset() noexcept { status_.fill(0); }

    // Return port * 16 + channel only when the PC data byte actually arrives.
    int feed(std::uint8_t byte, std::uint8_t port) noexcept {
        if (port >= status_.size() || byte >= 0xf8) return -1;
        if (byte & 0x80) {
            status_[port] = byte < 0xf0 ? byte : 0;
            return -1;
        }
        return (status_[port] & 0xf0) == 0xc0
                ? int(port) * 16 + (status_[port] & 0x0f) : -1;
    }

private:
    std::array<std::uint8_t, 4> status_{};
};

} // namespace rimu2000
