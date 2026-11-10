// SPDX-License-Identifier: BSD-3-Clause
// A skipped random sequence must leave the same state as drawing every value.
// This checks the public generator, including wraparound and the uncached path.
#include "mame/sound/swp30.h"

#include <array>
#include <cstdio>

int main()
{
	const std::array<u32, 8> seeds = {
		0, 1, 0xffffffffu, 0xfffffffeu,
		0x80000000u, 0x7fffffffu, 0x9d14abd7u, 0x12345678u
	};
	swp30_device drawn, skipped;
	u32 cases = 0;
	auto check = [&](u32 seed, u32 count) {
		drawn.set_rand_seed(seed);
		skipped.set_rand_seed(seed);
		for (u32 i = 0; i < count; ++i)
			drawn.rand();
		skipped.rand_skip(count);
		for (int i = 0; i < 4; ++i) {
			if (drawn.rand() != skipped.rand()) {
				std::fprintf(stderr, "LCG skip mismatch: seed=%08x count=%u next=%d\n",
				             seed, count, i);
				return false;
			}
		}
		++cases;
		return true;
	};
	for (u32 seed : seeds) {
		for (u32 count = 0; count <= 768; ++count)
			if (!check(seed, count))
				return 1;
		for (u32 count : {769u, 1024u, 4097u})
			if (!check(seed, count))
				return 1;
	}
	std::printf("LCG skip: %u cases match repeated draws (0..768, fallback, wraparound).\n", cases);
	return 0;
}
