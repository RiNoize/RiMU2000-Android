// SPDX-License-Identifier: BSD-3-Clause
// Synthetic RAM/ROM only: no firmware files or assets are required.
#include "../app/src/main/cpp/patch_snapshot.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>

using rimu2000::PartPatchData;
using rimu2000::PatchSnapshot;

namespace {
std::size_t checks = 0;
void require(bool ok, const char* description) {
    ++checks;
    if (!ok) {
        std::fprintf(stderr, "Patch snapshot failure: %s\n", description);
        std::exit(1);
    }
}

bool same(const PartPatchData& a, const PartPatchData& b) {
    return a.part == b.part && a.valid == b.valid && a.voice_record == b.voice_record
        && a.bank_msb == b.bank_msb && a.bank_lsb == b.bank_lsb && a.program == b.program
        && a.receive_channel == b.receive_channel && a.part_mode == b.part_mode && a.kit == b.kit;
}

void put_part(std::vector<std::uint8_t>& ram, const PartPatchData& data) {
    auto* block = ram.data() + xg::ram::part_base(data.part);
    block[1] = data.bank_msb;
    block[2] = data.bank_lsb;
    block[3] = data.program;
    block[4] = data.receive_channel;
    block[7] = data.part_mode;
    block[PatchSnapshot::kit_offset] = data.kit;
    for (unsigned i = 0; i < 4; ++i)
        block[xg::voice_rom::PART_VOICE + i] = std::uint8_t(data.voice_record >> (24 - 8 * i));
}

PartPatchData example(int part, int variant = 0) {
    PartPatchData data;
    data.part = part;
    data.valid = true;
    data.voice_record = PatchSnapshot::first_voice_record + std::uint32_t(part * 16 + variant * 2048);
    data.bank_msb = std::uint8_t(part + variant * 64);
    data.bank_lsb = std::uint8_t(127 - part - variant * 64);
    data.program = std::uint8_t((part * 3 + variant * 37) & 127);
    data.receive_channel = std::uint8_t((63 - part + variant * 7) & 63);
    data.part_mode = std::uint8_t((part + variant) % 6);
    data.kit = std::uint8_t(part * 3 + variant * 64);
    return data;
}

std::shared_ptr<std::vector<u8>> synthetic_rom() {
    auto rom = std::make_shared<std::vector<u8>>(0x400000, 0);
    const auto text = [&](std::size_t offset, const char* value, std::size_t size) {
        std::copy_n(reinterpret_cast<const u8*>(value), size, rom->data() + offset);
    };
    // Minimal layout signature used by voice_rom::ok(); all other bytes are
    // synthesized here, not copied from a firmware image.
    text(0x200ee0 + 2, "GrandPno  ", 10);
    text(0x299dc0, "StandKit", 8);
    text(0x29bdec, "SFXKit 1", 8);
    (*rom)[0x1bbf70] = 0xff;
    (*rom)[0x1bbf71] = 0xfc;
    text(0x200ee0 + 64 + 2, "BankedTest", 10);
    (*rom)[0x299fcc + 5] = 1;
    text(0x299dc0 + 12, "Alt Kit ", 8);
    (*rom)[0x299fcc + 6] = 2;
    text(0x299dc0 + 24, "SilenKit", 8);
    (*rom)[0x299fcc + 56] = 3;
    text(0x299dc0 + 36, "Ntrl Kit", 8);
    (*rom)[0x29be58 + 16] = 1;
    text(0x29bdec + 12, "FX Test ", 8);
    text(0x200ee0 + 128 + 2, "Bad\x01Name  ", 10);
    return rom;
}
} // namespace

int main() {
    // The physical order starts with channel 10 on every port.
    require(xg::ram::part_base(9) == xg::ram::PARTS, "A10 physical first slot");
    require(xg::ram::part_base(0) == xg::ram::PARTS + xg::ram::PART_STRIDE, "A1 physical second slot");
    require(xg::ram::part_base(15) == xg::ram::PARTS + 15 * xg::ram::PART_STRIDE, "A16 slot");
    require(xg::ram::part_base(25) == xg::ram::PARTS + 16 * xg::ram::PART_STRIDE, "B10 physical port start");
    require(xg::ram::part_base(16) == xg::ram::PARTS + 17 * xg::ram::PART_STRIDE, "B1 reordered slot");
    require(xg::ram::part_base(63) == xg::ram::PARTS + 63 * xg::ram::PART_STRIDE, "D16 last slot");

    std::vector<std::uint8_t> ram(0x40000, 0), alternate(0x40000, 0);
    std::array<PartPatchData, 64> expected{}, other{};
    for (int part = 0; part < 64; ++part) {
        expected[part] = example(part);
        other[part] = example(part, 1);
        put_part(ram, expected[part]);
        put_part(alternate, other[part]);
    }
    // Two parts can listen to the same route; neither overwrites the other.
    expected[5].receive_channel = expected[0].receive_channel;
    put_part(ram, expected[5]);
    PatchSnapshot snapshot;
    require(!snapshot.publish(ram) && !snapshot.load(0).valid, "unknown ROM layout stays unavailable");
    snapshot.configure(true);
    require(snapshot.layout_known() && !snapshot.load(0).valid, "configure discards old state");
    require(snapshot.publish(ram), "publish full RAM");
    for (int part = 0; part < 64; ++part) {
        require(same(snapshot.load(part), expected[part]), "bank/program/record/route capture");
        require(same(PatchSnapshot::capturePart(ram, part, true), expected[part]), "pure owner capture");
        require(same(PatchSnapshot::unpack(PatchSnapshot::pack(expected[part]), part), expected[part]), "atomic word roundtrip");
    }
    require(snapshot.load(0).receive_channel == snapshot.load(5).receive_channel,
            "shared receive route retains both parts");
    require(!snapshot.load(-1).valid && !snapshot.load(64).valid, "invalid part indices");
    require(!PatchSnapshot::capturePart(ram, -1, true).valid
        && !PatchSnapshot::capturePart(ram, 64, true).valid
        && !PatchSnapshot::capturePart(ram, 0, false).valid, "capture guards");
    const std::vector<std::uint8_t> short_ram(xg::ram::part_base(63) + PatchSnapshot::kit_offset, 0);
    require(!snapshot.publish(short_ram), "truncated RAM rejected");
    require(!snapshot.load(0).valid && !snapshot.load(63).valid, "truncated RAM clears stale patches");
    require(!PatchSnapshot::capturePart(short_ram, 63, true).valid, "kit byte bounds checked");

    auto edge = example(7);
    edge.bank_msb = edge.bank_lsb = edge.program = edge.receive_channel = 127;
    edge.part_mode = 5;
    edge.kit = 255;
    for (auto record : {PatchSnapshot::first_voice_record, PatchSnapshot::last_voice_record}) {
        edge.voice_record = record;
        require(same(PatchSnapshot::unpack(PatchSnapshot::pack(edge), 7), edge), "inclusive valid record bounds and maximum fields");
    }
    for (auto record : {0u, PatchSnapshot::first_voice_record - 1,
                        PatchSnapshot::last_voice_record + 1, 0xffffffffu, 0x40200ee0u}) {
        edge.voice_record = record;
        put_part(ram, edge);
        require(PatchSnapshot::capturePart(ram, 7, true).voice_record == 0, "invalid record read is unavailable");
        const auto decoded = PatchSnapshot::unpack(PatchSnapshot::pack(edge), 7);
        require(decoded.valid && decoded.voice_record == 0, "invalid record cannot wrap or alias during packing");
    }
    for (int field = 0; field < 5; ++field) {
        auto invalid = example(0);
        if (field == 0) invalid.bank_msb = 128;
        if (field == 1) invalid.bank_lsb = 255;
        if (field == 2) invalid.program = 128;
        if (field == 3) invalid.receive_channel = 64;
        if (field == 4) invalid.part_mode = 6;
        require(!PatchSnapshot::unpack(PatchSnapshot::pack(invalid)).valid, "invalid numeric fields rejected");
        put_part(ram, invalid);
        require(!PatchSnapshot::capturePart(ram, 0, true).valid, "invalid RAM fields rejected");
    }
    require(!PatchSnapshot::unpack(PatchSnapshot::pack(example(0)) | (std::uint64_t(1) << 63)).valid,
            "reserved bits rejected");

    const auto bytes = synthetic_rom();
    const xg::voice_rom rom(bytes);
    require(rom.ok(), "synthetic supported ROM layout");
    auto voice = example(0);
    voice.voice_record = PatchSnapshot::first_voice_record + 64;
    require(rimu2000::resolve_patch_name(voice, rom) == "BankedTest", "actual selected record takes priority over program/lookup tables");
    voice.voice_record = 0xffffffffu;
    require(rimu2000::resolve_patch_name(voice, rom).empty(), "overflowing record never dereferenced");
    voice.voice_record = PatchSnapshot::first_voice_record + 128;
    require(rimu2000::resolve_patch_name(voice, rom).empty(), "nonprintable record is unavailable");
    voice.voice_record = 0;
    voice.bank_msb = 127; voice.program = 5; voice.kit = 19;
    require(rimu2000::resolve_patch_name(voice, rom) == "Alt Kit", "drum name follows program map, not PART_KIT table order");
    voice.bank_msb = 0; voice.part_mode = 2;
    require(rimu2000::display_bank_msb(voice) == 127
        && rimu2000::resolve_patch_name(voice, rom) == "Alt Kit", "desktop GS drum mode mapping");
    voice.bank_msb = 126; voice.program = 16; voice.kit = 28;
    require(rimu2000::display_bank_msb(voice) == 126
        && rimu2000::resolve_patch_name(voice, rom) == "FX Test", "SFX drum bank remains distinct");
    voice.bank_msb = 0; voice.program = 56; voice.kit = 46;
    require(rimu2000::resolve_patch_name(voice, rom) == "SFX Set", "TG300B program 56 uses real kit 46, not XG name");
    voice.bank_msb = 127; voice.kit = 34;
    require(rimu2000::resolve_patch_name(voice, rom) == "Ntrl Kit", "XG program 56 verifies real kit 34");
    voice.kit = 36;
    require(rimu2000::resolve_patch_name(voice, rom).empty(), "kit/program mismatch does not guess an XG name");
    voice.kit = 0x80 | 34;
    require(rimu2000::resolve_patch_name(voice, rom).empty(), "drum-setup high bit is not a factory kit ID");
    voice.bank_msb = 127; voice.program = 6; voice.kit = 0;
    require(rimu2000::resolve_patch_name(voice, rom).empty(), "unmapped silent kit remains explicitly unknown");
    const xg::voice_rom unknown(std::make_shared<std::vector<u8>>(0x400000, 0));
    require(!unknown.ok() && rimu2000::resolve_patch_name(voice, unknown).empty(), "unknown ROM has no invented GM fallback");
    voice.valid = false;
    require(rimu2000::resolve_patch_name(voice, rom).empty(), "invalid snapshot has no name");

    // Concurrent publication can switch complete parts independently. A
    // reader must always get one whole tuple from either source RAM image.
    for (int part = 0; part < 64; ++part) put_part(ram, expected[part]);
    snapshot.configure(true);
    snapshot.publish(ram);
    std::atomic<bool> finished{false};
    std::thread writer([&] {
        for (int i = 0; i < 10000; ++i) {
            snapshot.publish(alternate);
            snapshot.publish(ram);
        }
        finished.store(true, std::memory_order_release);
    });
    std::size_t reads = 0;
    while (!finished.load(std::memory_order_acquire) || reads < 20000) {
        const int part = int(reads++ & 63);
        const auto data = snapshot.load(part);
        require(same(data, expected[part]) || same(data, other[part]), "concurrent reader sees one complete part tuple");
    }
    writer.join();
    snapshot.configure(false);
    require(!snapshot.layout_known() && !snapshot.load(0).valid && !snapshot.publish(ram), "stop/ROM change clears snapshot");
    std::printf("Patch snapshot: ordering, routes, bounds, firmware names and concurrent tuples passed (%zu checks).\n", checks);
}
