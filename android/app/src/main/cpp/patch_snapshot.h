// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include "xg/ram.h"
#include "xg/voices.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace rimu2000 {

// Firmware part numbers and receive routes are distinct. Both use 0..63 for
// A1..D16; receive_channel == 127 means OFF. Program numbers remain MIDI 0..127.
struct PartPatchData {
    int part = -1;
    std::uint32_t voice_record = 0;
    std::uint8_t bank_msb = 0, bank_lsb = 0, program = 0;
    std::uint8_t receive_channel = 127, part_mode = 0, kit = 0;
    bool valid = false;
};

// Values are the firmware's current MIDI-scale parameters, not UI defaults.
// Offsets follow xg/model.cpp: brightness, resonance, attack, release and sends.
struct PartControlData {
    enum Index { cutoff, resonance, attack, release, reverb, chorus, count };
    std::array<std::uint8_t, count> values{};
    bool valid = false;
};

class PatchSnapshot {
public:
    static constexpr int part_count = 64;
    static constexpr std::uint32_t first_voice_record = 0x200ee0;
    static constexpr std::uint32_t last_voice_record = 0x23cece - 16;
    static constexpr std::uint32_t kit_offset = 0x110;

    struct View {
        std::array<PartPatchData, part_count> parts{};
        std::array<PartControlData, part_count> controls{};
        int mode = -1, voiceSet = -1;
    };

    // Called before the emulation owner starts, with voice_rom::ok() evaluated
    // outside the audio callback. Changing ROMs also discards the old metadata.
    void configure(bool layout_known) noexcept {
        layout_known_.store(false, std::memory_order_release);
        clear();
        layout_known_.store(layout_known, std::memory_order_release);
    }

    void clear() noexcept {
        serial_.fetch_add(1, std::memory_order_acq_rel);
        for (auto& word : parts_) word.store(0, std::memory_order_release);
        for (auto& word : controls_) word.store(0, std::memory_order_release);
        context_.store(0, std::memory_order_release);
        serial_.fetch_add(1, std::memory_order_release);
    }

    bool layout_known() const noexcept {
        return layout_known_.load(std::memory_order_acquire);
    }

    // Bounds are checked before narrowing/packing. In particular, do not copy
    // upstream's unsigned r + 16 check: an arbitrary RAM value could wrap it.
    static constexpr bool valid_record(std::uint32_t record) noexcept {
        return record >= first_voice_record && record <= last_voice_record;
    }

    // Owner-thread-only RAM read. It sends no MIDI and allocates nothing. The
    // caller waits for firmware boot before using the result as current state.
    static PartPatchData capturePart(const std::vector<std::uint8_t>& ram,
                                     int part, bool layout_known) noexcept {
        PartPatchData data;
        if (part < 0 || part >= part_count) return data;
        data.part = part;
        if (!layout_known) return data;
        const std::size_t base = xg::ram::part_base(part);
        if (base >= ram.size() || ram.size() - base <= kit_offset) return data;
        const auto* block = ram.data() + base;
        data.bank_msb = block[0x01];
        data.bank_lsb = block[0x02];
        data.program = block[0x03];
        data.receive_channel = block[0x04];
        data.part_mode = block[0x07];
        data.kit = block[kit_offset];
        constexpr auto offset = xg::voice_rom::PART_VOICE;
        const auto record = (std::uint32_t(block[offset]) << 24)
            | (std::uint32_t(block[offset + 1]) << 16)
            | (std::uint32_t(block[offset + 2]) << 8) | block[offset + 3];
        data.voice_record = valid_record(record) ? record : 0;
        data.valid = valid_fields(data);
        return data;
    }

    static PartControlData captureControls(const std::vector<std::uint8_t>& ram,
                                           int part, bool layout_known) noexcept {
        PartControlData result;
        if (!layout_known || part < 0 || part >= part_count) return result;
        const std::size_t base = xg::ram::part_base(part);
        constexpr std::array<unsigned, PartControlData::count> offsets{
            0x18, 0x19, 0x1a, 0x1c, 0x13, 0x12};
        if (base >= ram.size() || ram.size() - base <= 0x1c) return result;
        for (unsigned i = 0; i < offsets.size(); ++i) {
            result.values[i] = ram[base + offsets[i]];
            if (result.values[i] > 127) return result;
        }
        result.valid = true;
        return result;
    }

    static std::uint64_t packControls(const PartControlData& data) noexcept {
        if (!data.valid) return 0;
        std::uint64_t word = std::uint64_t(1) << 42;
        for (unsigned i = 0; i < data.values.size(); ++i) {
            if (data.values[i] > 127) return 0;
            word |= std::uint64_t(data.values[i]) << (i * 7);
        }
        return word;
    }

    static PartControlData unpackControls(std::uint64_t word) noexcept {
        PartControlData data;
        if (!(word & (std::uint64_t(1) << 42)) || word >> 43) return data;
        for (unsigned i = 0; i < data.values.size(); ++i)
            data.values[i] = std::uint8_t((word >> (i * 7)) & 127);
        data.valid = true;
        return data;
    }

    // Each part is one atomic word, so a reader never combines the bank from
    // one update with the record/program from another. Parts need not be from
    // one global instant. The slot/part index is supplied separately to unpack.
    static std::uint64_t pack(const PartPatchData& data) noexcept {
        if (!data.valid || !valid_fields(data)) return 0;
        const std::uint64_t record = valid_record(data.voice_record) ? data.voice_record : 0;
        return record
            | (std::uint64_t(data.bank_msb) << 22)
            | (std::uint64_t(data.bank_lsb) << 29)
            | (std::uint64_t(data.program) << 36)
            | (std::uint64_t(data.receive_channel) << 43)
            | (std::uint64_t(data.part_mode) << 50)
            | (std::uint64_t(data.kit) << 53)
            | valid_bit;
    }

    static PartPatchData unpack(std::uint64_t word, int part = -1) noexcept {
        PartPatchData data;
        data.part = part;
        if (!(word & valid_bit) || (word >> 62)) return data;
        const auto record = std::uint32_t(word & ((std::uint64_t(1) << 22) - 1));
        data.voice_record = valid_record(record) ? record : 0;
        data.bank_msb = std::uint8_t((word >> 22) & 127);
        data.bank_lsb = std::uint8_t((word >> 29) & 127);
        data.program = std::uint8_t((word >> 36) & 127);
        data.receive_channel = std::uint8_t((word >> 43) & 127);
        data.part_mode = std::uint8_t((word >> 50) & 7);
        data.kit = std::uint8_t((word >> 53) & 255);
        data.valid = valid_fields(data);
        return data;
    }

    // Call on the mu2000 owner thread, e.g. once per 200--250 ms of emulated
    // audio. UI/JNI reads only load(), never the live mu2000::nvram() vector.
    bool publish(const std::vector<std::uint8_t>& ram) noexcept {
        const bool known = layout_known();
        if (!known || ram.size() <= xg::ram::part_base(part_count - 1) + kit_offset) {
            clear();
            return false;
        }
        serial_.fetch_add(1, std::memory_order_acq_rel);
        for (int part = 0; part < part_count; ++part) {
            parts_[part].store(pack(capturePart(ram, part, true)), std::memory_order_release);
            controls_[part].store(packControls(captureControls(ram, part, true)),
                                  std::memory_order_release);
        }
        context_.store(0x10000u | std::uint32_t(ram[xg::ram::VOICE_MODE])
                       | (std::uint32_t(ram[xg::ram::VOICE_SET]) << 8),
                       std::memory_order_release);
        serial_.fetch_add(1, std::memory_order_release);
        return true;
    }

    PartPatchData load(int part) const noexcept {
        if (part < 0 || part >= part_count) return {};
        return unpack(parts_[part].load(std::memory_order_acquire), part);
    }

    // A bounded, coherent read keeps controls and catalog context aligned with
    // the patch update. All payload words are atomic, including on a retry.
    // Audio never waits for readers and never allocates while publishing.
    bool read(View& view) const noexcept {
        for (int attempt = 0; attempt < 2; ++attempt) {
            const auto before = serial_.load(std::memory_order_acquire);
            if (before & 1) continue;
            const auto context = context_.load(std::memory_order_acquire);
            for (int part = 0; part < part_count; ++part) {
                view.parts[part] = unpack(parts_[part].load(std::memory_order_acquire), part);
                view.controls[part] = unpackControls(controls_[part].load(std::memory_order_acquire));
            }
            std::atomic_thread_fence(std::memory_order_acquire);
            if (before == serial_.load(std::memory_order_relaxed)) {
                view.mode = context & 0x10000u ? int(context & 255u) : -1;
                view.voiceSet = context & 0x10000u ? int((context >> 8) & 255u) : -1;
                return (context & 0x10000u) != 0;
            }
        }
        view = View{};
        return false;
    }

    // Mode and set are one atomic pair, usable as a cache key outside audio.
    std::array<int, 2> context() const noexcept {
        const auto word = context_.load(std::memory_order_acquire);
        if (!(word & 0x10000u)) return {-1, -1};
        return {int(word & 255u), int((word >> 8) & 255u)};
    }

    static constexpr bool valid_fields(const PartPatchData& data) noexcept {
        return data.bank_msb < 128 && data.bank_lsb < 128 && data.program < 128
            && (data.receive_channel < part_count || data.receive_channel == 127)
            && data.part_mode <= 5;
    }

private:
    static constexpr std::uint64_t valid_bit = std::uint64_t(1) << 61;
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
                  "Patch snapshots require lock-free 64-bit atomics");
    std::atomic<bool> layout_known_{false};
    std::atomic<std::uint64_t> serial_{0};
    std::atomic<std::uint32_t> context_{0};
    std::array<std::atomic<std::uint64_t>, part_count> parts_{};
    std::array<std::atomic<std::uint64_t>, part_count> controls_{};
};

// Match the upstream desktop's shown_bank_msb(): GS drums can retain MSB 0
// while the firmware part mode selects drums. SFX bank 126 stays distinct.
inline int display_bank_msb(const PartPatchData& data) noexcept {
    if (data.bank_msb == 126 || data.bank_msb == 127) return data.bank_msb;
    return data.part_mode != 0 ? 127 : data.bank_msb;
}

// UI/control-thread-only: string allocation and ROM lookup never run in the
// callback. The caller retains the immutable ROM (shared_ptr) for this call.
// Empty means unavailable; never substitute a guessed GM name for firmware.
inline std::string resolve_patch_name(const PartPatchData& data, const xg::voice_rom& rom) {
    if (!data.valid || !PatchSnapshot::valid_fields(data) || !rom.ok()) return {};
    const int msb = display_bank_msb(data);
    std::string name;
    if (msb == 126 || msb == 127) {
        // PART_KIT is the firmware's actual selection. Bit 7 selects the
        // separate drum-setup path (voice_rom::drum_record), so interpreting
        // those values as factory kit IDs could give a misleading name.
        if (data.kit & 0x80) return {};
        // These TG300B IDs/names were measured from firmware by upstream.
        // Its program 56 is kit 46/SFX Set; XG program 56 is kit 34/Ntrl Kit.
        for (const auto& kit : xg::voice_rom::TG300B_KITS)
            if (kit.kit == data.kit) { name = kit.name; break; }
        if (name.empty()) {
            // Verify the real kit before consulting the XG/SFX name table.
            // Unmapped, silent or mismatched IDs remain explicitly unknown.
            if (rom.kit_number(msb, data.program) != data.kit) return {};
            name = rom.kit_name(msb, data.program);
        }
    } else {
        std::array<u8, xg::voice_rom::PART_VOICE + 4> block{};
        const auto record = PatchSnapshot::valid_record(data.voice_record) ? data.voice_record : 0;
        for (unsigned i = 0; i < 4; ++i)
            block[xg::voice_rom::PART_VOICE + i] = std::uint8_t(record >> (24 - i * 8));
        // The melodic record follows the firmware's actual fallback choice.
        name = rom.name(block.data(), msb, data.program);
    }
    for (unsigned char c : name)
        if (c < 0x20 || c > 0x7e) return {};
    return name;
}

} // namespace rimu2000
