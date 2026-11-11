// SPDX-License-Identifier: BSD-3-Clause
// Entirely synthetic tables; no firmware bytes or external ROM are required.
#include "../app/src/main/cpp/voice_catalog.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace {

#define REQUIRE(condition) do { if (!(condition)) throw std::runtime_error( \
    std::string(__func__) + ":" + std::to_string(__LINE__) + ": " #condition); } while (false)

// Only the layout and the four public recognition sentinels come from
// voice_rom's API. Every bank, record and kit used below is invented.
struct SyntheticROM {
    static constexpr std::uint32_t voices = 0x200ee0;
    static constexpr std::uint32_t table = 0x267f50;
    static constexpr std::uint32_t groupGM = 0x283d50, groupXG = 0x283950;
    static constexpr std::uint32_t lsb0 = 0x2839d0, lsb1 = 0x283a50;
    static constexpr std::uint32_t lsb77 = 0x283ad0;
    static constexpr std::uint32_t lsbC9_0 = 0x292640, lsbC9_1 = 0x2926c0;
    static constexpr std::uint32_t kitMap = 0x299fcc, kitNames = 0x299dc0;
    static constexpr std::uint32_t sfxMap = 0x29be58, sfxNames = 0x29bdec;

    static constexpr std::uint32_t a = voices + 0x40, b = voices + 0x80;
    static constexpr std::uint32_t c = voices + 0xc0, twin1 = voices + 0x100;
    static constexpr std::uint32_t twin2 = voices + 0x140, silence = voices + 0x180;
    static constexpr std::uint32_t bad = voices + 0x1c0, blank = voices + 0x200;
    static constexpr std::uint32_t fx = voices + 0x240, set1a = voices + 0x280;
    static constexpr std::uint32_t set1b = voices + 0x2c0, g30 = voices + 0x300;
    static constexpr std::uint32_t g31 = voices + 0x340, g40 = voices + 0x380;
    static constexpr std::uint32_t g41 = voices + 0x3c0, g50 = voices + 0x400;
    static constexpr std::uint32_t g51 = voices + 0x440, gm = voices + 0x480;
    static constexpr std::uint32_t gmA = voices + 0x4c0, gmB = voices + 0x500;
    static constexpr std::uint32_t excludedGM = voices + 0x540;

    std::shared_ptr<std::vector<u8>> bytes = std::make_shared<std::vector<u8>>(0x400000, 0);

    explicit SyntheticROM(bool populate = true) {
        // An out-of-range record offset makes unpopulated program slots
        // invalid, rather than accidentally mapping every slot to GrandPno.
        for (int group = 0; group < 256; ++group)
            for (int program = 0; program < 128; ++program)
                put32(table + std::uint32_t(group * 512 + program * 4), 0x00100000);
        fill(groupGM, 0xff);
        fill(groupXG, 250);
        fill(lsb0, 10); fill(lsb1, 20); fill(lsb77, 30);
        fill(lsbC9_0, 40); fill(lsbC9_1, 50);
        fill(kitMap, 1); fill(sfxMap, 1);
        text(voices + 2, "GrandPno", 10);
        text(kitNames, "StandKit", 8);
        text(sfxNames, "SFXKit 1", 8);
        text(kitNames + 12, "SilenKit", 8);
        text(sfxNames + 12, "SilenKit", 8);
        (*bytes)[0x1bbf70] = 0xff;
        (*bytes)[0x1bbf71] = 0xfc;
        if (!populate) return;

        record(a, "Base One"); record(b, "Variation"); record(c, "Other PC");
        record(twin1, "TwinName"); record(twin2, "TwinName");
        record(silence, "Silence"); record(bad, std::string("Bad") + char(1));
        record(blank, ""); record(fx, "Rain Test");
        record(set1a, "Set Two A"); record(set1b, "Set Two B");
        record(g30, "Kind77 A"); record(g31, "Kind77 B");
        record(g40, "KindC9 A"); record(g41, "KindC9 B");
        record(g50, "SetC9 A"); record(g51, "SetC9 B");
        record(gm, "GM Default"); record(gmA, "GM Bank A");
        record(gmB, "GM Bank B"); record(excludedGM, "Kit MSB");

        (*bytes)[groupXG + 0] = 0;
        (*bytes)[groupXG + 1] = 0; // Equivalent complete bank.
        (*bytes)[groupXG + 2] = 77;
        (*bytes)[groupXG + 3] = 0xc9;
        (*bytes)[groupXG + 4] = 11;
        (*bytes)[groupXG + 5] = 60; // Different group, repeated records.
        (*bytes)[groupXG + 16] = 0;
        (*bytes)[groupXG + 64] = 70;
        (*bytes)[lsb0 + 1] = 11;
        (*bytes)[lsb0 + 2] = 12;
        (*bytes)[lsb1 + 1] = 21;
        (*bytes)[lsb77 + 4] = 31;
        (*bytes)[lsbC9_0 + 5] = 41;
        (*bytes)[lsbC9_1 + 5] = 51;

        slot(10, 0, a); slot(10, 1, c); slot(10, 8, a);
        slot(10, 9, silence); slot(10, 10, bad); slot(10, 11, blank);
        slot(10, 12, 0xfffffff8U); // Corrupt record whose 32-bit end address wraps.
        // Groups 10 and 11 agree at program 0 but differ at program 1.
        slot(11, 0, a); slot(11, 1, b); slot(11, 9, twin1);
        slot(12, 9, twin2);
        slot(60, 0, a); slot(60, 1, b); slot(60, 9, twin1);
        slot(20, 0, set1a); slot(21, 0, set1b);
        slot(30, 16, g30); slot(31, 17, g31);
        slot(40, 24, g40); slot(41, 25, g41);
        slot(50, 24, g50); slot(51, 25, g51);
        slot(70, 0, fx);

        (*bytes)[groupGM + 1] = 80;
        (*bytes)[groupGM + 2] = 80;
        (*bytes)[groupGM + 3] = 81;
        (*bytes)[groupGM + 64] = 70;
        (*bytes)[groupGM + 126] = 82;
        slot(76, 0, gm); slot(76, 8, a);
        slot(80, 0, gmA); slot(81, 0, gmA); slot(81, 1, gmB);
        slot(82, 0, excludedGM);

        text(kitNames + 2 * 12, "Twin Kit", 8);
        text(kitNames + 3 * 12, "Twin Kit", 8);
        text(kitNames + 4 * 12, std::string("Bad") + char(1), 8);
        text(kitNames + 5 * 12, "Silence", 8);
        text(kitNames + 6 * 12, "", 8);
        (*bytes)[kitMap + 0] = 0;
        (*bytes)[kitMap + 1] = 2;
        (*bytes)[kitMap + 2] = 3;
        (*bytes)[kitMap + 3] = 4;
        (*bytes)[kitMap + 4] = 5;
        (*bytes)[kitMap + 5] = 6;
        (*bytes)[kitMap + 126] = 0; // Deliberate address alias of Standard.
        text(sfxNames + 2 * 12, "Test SFX", 8);
        (*bytes)[sfxMap + 0] = 0;
        (*bytes)[sfxMap + 1] = 2;
        (*bytes)[sfxMap + 2] = 2;
    }

    void fill(std::uint32_t address, u8 value) {
        std::fill_n(bytes->begin() + address, 128, value);
    }
    void text(std::uint32_t address, const std::string& value, std::size_t width) {
        if (value.size() > width) throw std::runtime_error("synthetic name too long");
        std::fill_n(bytes->begin() + address, width, u8(' '));
        std::copy(value.begin(), value.end(), bytes->begin() + address);
    }
    void put32(std::uint32_t address, std::uint32_t value) {
        for (int i = 0; i < 4; ++i)
            (*bytes)[address + std::uint32_t(i)] = u8(value >> (24 - i * 8));
    }
    void record(std::uint32_t address, const std::string& name) {
        text(address + 2, name, 10);
    }
    void slot(int group, int program, std::uint32_t address) {
        put32(table + std::uint32_t(group * 512 + program * 4), (address - voices) / 2);
    }
    xg::voice_rom rom() const { return xg::voice_rom(bytes); }
};

using Entry = rimu::VoiceCatalogEntry;
using Catalog = rimu::VoiceCatalog;

const Entry* at(const Catalog& catalog, int msb, int lsb, int program) {
    const auto it = std::find_if(catalog.entries.begin(), catalog.entries.end(), [&](const Entry& e) {
        return e.msb == msb && e.lsb == lsb && e.program == program;
    });
    return it == catalog.entries.end() ? nullptr : &*it;
}

std::size_t countKind(const Catalog& catalog, int kind) {
    return std::size_t(std::count_if(catalog.entries.begin(), catalog.entries.end(),
                                   [=](const Entry& e) { return e.kind == kind; }));
}

auto identity(const Entry& entry) {
    return std::tie(entry.kind, entry.family, entry.msb, entry.lsb,
                    entry.program, entry.record, entry.name);
}

void recognitionAndEmptyMappings() {
    const SyntheticROM empty(false);
    const auto rom = empty.rom();
    REQUIRE(rom.ok());
    for (int mode : {0, 1, 2}) {
        const auto catalog = rimu::buildVoiceCatalog(rom, mode, 0);
        REQUIRE(catalog.recognized && catalog.omittedSpecialBanks);
        REQUIRE(catalog.mode == mode && catalog.voiceSet == 0);
        REQUIRE(catalog.entries.empty());
    }

    const xg::voice_rom nullROM(nullptr);
    const auto absent = rimu::buildVoiceCatalog(nullROM, 1, 1);
    REQUIRE(!absent.recognized && !absent.omittedSpecialBanks && absent.entries.empty());
    REQUIRE(nullROM.bank_group(1, 0, 0, 0) == -1);
    REQUIRE(nullROM.lookup(1, 0, 0, 0, 0) == 0);

    auto altered = std::make_shared<std::vector<u8>>(*empty.bytes);
    (*altered)[0x1bbf70] = 0;
    REQUIRE(!rimu::buildVoiceCatalog(xg::voice_rom(altered), 1, 0).recognized);
    altered->resize(0x3fffff);
    REQUIRE(!rimu::buildVoiceCatalog(xg::voice_rom(altered), 0, 0).recognized);
}

void groupResolutionPreservesLookupSemantics() {
    const SyntheticROM fixture;
    const auto rom = fixture.rom();
    REQUIRE(rom.bank_group(0, 0, 0, 0) == 76); // 0xff fallback is GM-only.
    REQUIRE(rom.bank_group(0, 1, 1, 127) == 80);
    REQUIRE(rom.bank_group(1, 0, 0, 0) == 10);
    REQUIRE(rom.bank_group(1, 0, 0, 1) == 11);
    REQUIRE(rom.bank_group(1, 1, 0, 0) == 20);
    REQUIRE(rom.bank_group(1, 2, 0, 1) == 21); // Existing nonzero-set behavior.
    REQUIRE(rom.bank_group(1, 0, 2, 0) == 30);
    REQUIRE(rom.bank_group(1, 1, 2, 4) == 31);
    REQUIRE(rom.bank_group(1, 0, 3, 5) == 41);
    REQUIRE(rom.bank_group(1, 1, 3, 5) == 51);
    REQUIRE(rom.bank_group(1, 0, 4, 127) == 11); // Literal group ignores LSB.
    REQUIRE(rom.bank_group(2, 1, 0, 0) == 76); // Legacy lookup fallback retained.
    REQUIRE(rom.bank_group(1, 0, 16, 0) == -1);
    REQUIRE(rom.bank_group(1, 0, 16, 1) == -1);
    REQUIRE(rom.lookup(1, 0, 16, 0, 0) == 0);
    REQUIRE(rom.lookup(1, 0, 16, 1, 1) == 0);
    REQUIRE(rom.bank_group(1, 0, 16, 2) == 12);
    REQUIRE(rom.bank_group(1, 0, 128, 129) == 11);
    REQUIRE(rom.bank_group(1, 0, -124, -1) == 11);
    REQUIRE(rom.lookup(1, 0, 128, 129, 129) == SyntheticROM::b);
    REQUIRE(rom.lookup(1, 0, 0, 1, 127 + 2) == SyntheticROM::b);
    REQUIRE(rom.lookup(1, 0, 0, 0, 127) == 0);
    REQUIRE(rom.lookup(0, 0, 0, 0, 0) == SyntheticROM::gm);
    REQUIRE(rom.lookup(2, 0, 0, 0, 0) == SyntheticROM::gm);
    REQUIRE(!rom.lsb_ok(1, 0, 0, 3));
    REQUIRE(rom.lookup(1, 0, 0, 3, 0) == SyntheticROM::a); // lookup does not reject ignored LSB.
}

void completeGroupsAndRecordIdentity() {
    const SyntheticROM fixture;
    const auto catalog = rimu::buildVoiceCatalog(fixture.rom(), 1, 0);
    REQUIRE(countKind(catalog, 0) == 11);
    const Entry* base = at(catalog, 0, 0, 0);
    const Entry* variation = at(catalog, 0, 1, 1);
    REQUIRE(base && base->record == SyntheticROM::a);
    REQUIRE(variation && variation->record == SyntheticROM::b);
    REQUIRE(!at(catalog, 0, 1, 0)); // Same program/record has one canonical bank.
    REQUIRE(!at(catalog, 1, 0, 0)); // Complete-group alias is not repeated.
    REQUIRE(!at(catalog, 5, 0, 1)); // Distinct group with the same program/record.
    REQUIRE(at(catalog, 0, 0, 8)); // Same record at a DIFFERENT program survives.
    REQUIRE(at(catalog, 0, 0, 8)->record == base->record);
    REQUIRE(at(catalog, 0, 0, 8)->family == 1);
    REQUIRE(!at(catalog, 0, 0, 12)); // Invalid record is bounded before decoding its name.
    const Entry* twin1 = at(catalog, 0, 1, 9);
    const Entry* twin2 = at(catalog, 0, 2, 9);
    REQUIRE(twin1 && twin2 && twin1->name == twin2->name);
    REQUIRE(twin1->record != twin2->record); // Display name is not identity.
    REQUIRE(at(catalog, 2, 4, 17) && at(catalog, 3, 5, 25));
    const Entry* effects = at(catalog, 64, 0, 0);
    REQUIRE(effects && effects->kind == 0 && effects->family == 17);
    for (const auto& entry : catalog.entries) {
        REQUIRE(entry.name != "Silence" && !entry.name.empty());
        REQUIRE(entry.msb != 16); // Special bank is never fabricated.
        REQUIRE(entry.msb != 0 || entry.lsb <= 2); // Ignored LSBs are not selectable entries.
        REQUIRE(entry.record != SyntheticROM::bad && entry.record != SyntheticROM::blank);
    }
}

void voiceSetChangesTheActualBank() {
    const SyntheticROM fixture;
    const auto catalog = rimu::buildVoiceCatalog(fixture.rom(), 1, 1);
    REQUIRE(catalog.mode == 1 && catalog.voiceSet == 1);
    REQUIRE(countKind(catalog, 0) == 10);
    REQUIRE(at(catalog, 0, 0, 0) && at(catalog, 0, 0, 0)->record == SyntheticROM::set1a);
    REQUIRE(at(catalog, 0, 1, 0) && at(catalog, 0, 1, 0)->record == SyntheticROM::set1b);
    REQUIRE(at(catalog, 4, 0, 1) && at(catalog, 4, 0, 1)->record == SyntheticROM::b);
    REQUIRE(at(catalog, 3, 0, 24) && at(catalog, 3, 0, 24)->record == SyntheticROM::g50);
    REQUIRE(at(catalog, 3, 5, 25) && at(catalog, 3, 5, 25)->record == SyntheticROM::g51);
    REQUIRE(!at(catalog, 0, 2, 9));
}

void gmFallbackDoesNotInventXGKits() {
    const SyntheticROM fixture;
    const auto catalog = rimu::buildVoiceCatalog(fixture.rom(), 0, 0);
    const auto otherSet = rimu::buildVoiceCatalog(fixture.rom(), 0, 1);
    REQUIRE(catalog.recognized && catalog.omittedSpecialBanks);
    REQUIRE(catalog.entries.size() == 5 && countKind(catalog, 0) == 5);
    REQUIRE(at(catalog, 0, 0, 0) && at(catalog, 0, 0, 0)->record == SyntheticROM::gm);
    REQUIRE(at(catalog, 1, 0, 0) && at(catalog, 1, 0, 0)->record == SyntheticROM::gmA);
    REQUIRE(at(catalog, 3, 0, 1) && at(catalog, 3, 0, 1)->record == SyntheticROM::gmB);
    REQUIRE(at(catalog, 64, 0, 0) && at(catalog, 64, 0, 0)->family == 17);
    REQUIRE(catalog.entries.size() == otherSet.entries.size());
    for (std::size_t i = 0; i < catalog.entries.size(); ++i) {
        REQUIRE(identity(catalog.entries[i]) == identity(otherSet.entries[i]));
        REQUIRE(catalog.entries[i].lsb == 0 && catalog.entries[i].msb < 126);
        REQUIRE(catalog.entries[i].record != SyntheticROM::excludedGM);
    }
    REQUIRE(rimu::buildVoiceCatalog(fixture.rom(), 2, 0).entries.empty());
}

void kitsKeepTheirMidiAddresses() {
    const SyntheticROM fixture;
    const auto catalog = rimu::buildVoiceCatalog(fixture.rom(), 1, 0);
    REQUIRE(countKind(catalog, 1) == 4);
    REQUIRE(countKind(catalog, 2) == 3);
    const Entry* standard = at(catalog, 127, 0, 0);
    const Entry* alias = at(catalog, 127, 0, 126);
    REQUIRE(standard && alias && standard->name == alias->name);
    REQUIRE(standard->record == 0 && alias->record == 0);
    REQUIRE(at(catalog, 127, 0, 1) && at(catalog, 127, 0, 2));
    REQUIRE(at(catalog, 127, 0, 1)->name == at(catalog, 127, 0, 2)->name);
    REQUIRE(at(catalog, 126, 0, 0) && at(catalog, 126, 0, 1) && at(catalog, 126, 0, 2));
    REQUIRE(!at(catalog, 127, 0, 3) && !at(catalog, 127, 0, 4) && !at(catalog, 127, 0, 5));
    REQUIRE(!at(catalog, 127, 0, 6)); // SilenKit, the default missing-kit entry.
    for (const auto& entry : catalog.entries) {
        if (!entry.kind) continue;
        REQUIRE(entry.lsb == 0 && entry.record == 0);
        REQUIRE((entry.kind == 1 && entry.msb == 127 && entry.family == 16) ||
                (entry.kind == 2 && entry.msb == 126 && entry.family == 17));
    }
}

void everyEntryRoundTripsAndOrderIsStable() {
    const SyntheticROM fixture;
    const auto rom = fixture.rom();
    for (int mode : {0, 1}) {
        for (int set : {0, 1}) {
            const auto first = rimu::buildVoiceCatalog(rom, mode, set);
            const auto second = rimu::buildVoiceCatalog(rom, mode, set);
            REQUIRE(first.entries.size() == second.entries.size());
            std::set<std::tuple<int, int, int>> addresses;
            std::set<std::pair<int, std::uint32_t>> melodicRecords;
            for (std::size_t i = 0; i < first.entries.size(); ++i) {
                const auto& entry = first.entries[i];
                REQUIRE(identity(entry) == identity(second.entries[i]));
                REQUIRE(addresses.emplace(entry.msb, entry.lsb, entry.program).second);
                if (entry.kind == 0) {
                    REQUIRE(melodicRecords.emplace(entry.program, entry.record).second);
                    REQUIRE(rom.lsb_ok(mode, set, entry.msb, entry.lsb));
                    REQUIRE(rom.lookup(mode, set, entry.msb, entry.lsb, entry.program) == entry.record);
                    REQUIRE(rom.record_name(entry.record) == entry.name);
                } else {
                    REQUIRE(rom.kit_name(entry.msb, entry.program) == entry.name);
                }
            }
        }
    }
}

} // namespace

int main() {
    try {
        recognitionAndEmptyMappings();
        groupResolutionPreservesLookupSemantics();
        completeGroupsAndRecordIdentity();
        voiceSetChangesTheActualBank();
        gmFallbackDoesNotInventXGKits();
        kitsKeepTheirMidiAddresses();
        everyEntryRoundTripsAndOrderIsStable();
        std::cout << "voice_catalog: 7 test groups passed (synthetic ROM only)\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
