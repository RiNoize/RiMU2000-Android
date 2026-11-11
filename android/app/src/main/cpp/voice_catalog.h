// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include "xg/voices.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace rimu {

struct VoiceCatalogEntry {
    // kind: 0 melodic, 1 drum kit, 2 SFX kit.
    // family: 0..15 GM program families, 16 drums, 17 effects. The melodic
    // effects bank (MSB 64) belongs to family 17 but remains kind 0.
    int kind = 0, family = 0;
    int msb = 0, lsb = 0, program = 0; // MIDI numbers, all zero-based.
    std::uint32_t record = 0;          // Zero for kits; not a persistent row ID.
    std::string name;
};

struct VoiceCatalog {
    int mode = 0, voiceSet = 0;
    bool recognized = false;
    // The browser covers the mappings implemented by voice_rom, not every
    // firmware path. XG's special MSB 16/LSB 0..1 is omitted. Mode 0 has only
    // melodic mappings here; its kits are not inferred from XG kit tables.
    // Other modes are left empty instead of guessing their selectable banks.
    bool omittedSpecialBanks = false;
    std::vector<VoiceCatalogEntry> entries;
};

namespace voice_catalog_detail {
inline bool printableName(const std::string& name) noexcept {
    if (name.empty()) return false;
    return std::all_of(name.begin(), name.end(), [](unsigned char c) {
        return c >= 0x20 && c <= 0x7e;
    });
}
} // namespace voice_catalog_detail

// UI/control-thread-only. This allocates strings/vectors and must never be
// called in an audio callback. The caller owns an immutable voice_rom for the
// duration of this call and keys any cache by ROM identity, mode, and set.
// Selection/favorites use the full context + kind/MSB/LSB/program, not a row
// index: aliases and catalogue ordering can differ between firmware modes.
inline VoiceCatalog buildVoiceCatalog(const xg::voice_rom& rom, int mode, int set) {
    VoiceCatalog result;
    result.mode = mode;
    result.voiceSet = set;
    result.recognized = rom.ok();
    if (!result.recognized) return result;
    result.omittedSpecialBanks = true;
    if (mode != 0 && mode != 1) return result;

    struct Bank { int msb, lsb; };
    std::vector<Bank> banks;
    banks.reserve(256);
    std::array<bool, 256> seenGroups{};
    // Ascending addresses preserve the desktop's canonical-bank preference.
    // Equivalent groups have identical mappings for ALL 128 programs. We do
    // not infer bank equivalence from a single sample program.
    for (int msb = 0; msb < 126; ++msb) {
        const int lsbCount = mode == 1 ? 128 : 1;
        for (int lsb = 0; lsb < lsbCount; ++lsb) {
            if (!rom.lsb_ok(mode, set, msb, lsb)) continue;
            const int group = rom.bank_group(mode, set, msb, lsb);
            if (group < 0 || group >= int(seenGroups.size()) || seenGroups[std::size_t(group)])
                continue;
            seenGroups[std::size_t(group)] = true;
            banks.push_back({msb, lsb});
        }
    }

    result.entries.reserve(2048);
    for (int program = 0; program < 128; ++program) {
        std::array<std::uint32_t, 256> seenRecords{};
        std::size_t seenCount = 0;
        for (const auto& bank : banks) {
            const auto record = rom.lookup(mode, set, bank.msb, bank.lsb, program);
            // ok() recognizes a 4 MiB layout. Bound the complete record
            // before calling the legacy name reader, whose 32-bit address
            // addition could wrap on a corrupt table offset.
            if (!record || std::uint64_t(record) + 16 > 0x400000ULL) continue;
            if (std::find(seenRecords.begin(), seenRecords.begin() + seenCount, record)
                    != seenRecords.begin() + seenCount)
                continue;
            auto name = rom.record_name(record);
            if (!voice_catalog_detail::printableName(name) || name == "Silence") continue;
            seenRecords[seenCount++] = record;
            const int family = bank.msb == 64 ? 17 : program / 8;
            result.entries.push_back({0, family, bank.msb, bank.lsb, program, record, std::move(name)});
        }
    }

    if (mode == 1) {
        // Keep kit addresses, including aliases. Their record is zero, and
        // equal display names do not establish equal kit identities.
        for (int msb : {127, 126}) {
            for (int program = 0; program < 128; ++program) {
                auto name = rom.kit_name(msb, program);
                if (!voice_catalog_detail::printableName(name) || name == "Silence") continue;
                result.entries.push_back({msb == 127 ? 1 : 2, msb == 127 ? 16 : 17,
                                          msb, 0, program, 0, std::move(name)});
            }
        }
    }
    return result;
}

} // namespace rimu
