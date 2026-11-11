// SPDX-License-Identifier: BSD-3-Clause
// Host-only selection tests: no actual process/thread affinity is modified.
#include "../app/src/main/cpp/audio_cpu_placement.h"

#include <cstdio>
#include <cstdlib>
#include <initializer_list>

using namespace rimu2000::cpu_placement;

static void require(bool value, const char* message) {
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

static cpu_set_t mask(std::initializer_list<int> cpus) {
    cpu_set_t result{};
    for (const int cpu : cpus) CPU_SET(cpu, &result);
    return result;
}

static void cpu(Topology& topology, int index, int capacity, int maximumKhz) {
    topology[index] = {true, capacity, maximumKhz};
}

static Selection select(const Topology& topology, const cpu_set_t& allowed) {
    const auto original = allowed;
    const auto result = selectPerformance(topology, allowed);
    require(CPU_EQUAL(&original, &allowed), "selection preserves the original mask");
    for (int index = 0; index < CPU_SETSIZE; ++index)
        require(!CPU_ISSET(index, &result.requested) || CPU_ISSET(index, &allowed),
                "requested mask must be a subset of allowed CPUs");
    return result;
}

int main() {
    int scenarios = 0;
    {
        Topology topology{};
        cpu(topology, 0, 1024, 2200000);
        cpu(topology, 3, 383, 2000000);
        cpu(topology, 7, 1024, 2200000);
        const auto result = select(topology, mask({0, 3, 7}));
        const auto expected = mask({0, 7});
        require(result.change && result.criterion == Criterion::capacity,
                "complete capacity selects performance cores");
        require(CPU_EQUAL(&result.requested, &expected),
                "all tied maxima, including low and noncontiguous IDs, are selected");
        ++scenarios;
    }
    {
        Topology topology{};
        cpu(topology, 0, 1024, 2200000);
        cpu(topology, 2, 383, 2000000);
        cpu(topology, 5, 383, 2000000);
        const auto result = select(topology, mask({2, 5}));
        require(!result.change && result.reason == Reason::uniformCapacity,
                "excluded performance cores are never added to the allowed mask");
        ++scenarios;
    }
    {
        Topology topology{};
        cpu(topology, 0, 1024, 2200000);
        cpu(topology, 2, 383, 2000000);
        cpu(topology, 5, 1024, 2200000);
        const auto result = select(topology, mask({0, 2}));
        const auto expected = mask({0});
        require(result.change && CPU_EQUAL(&result.requested, &expected),
                "one permitted performance core is a valid selection");
        ++scenarios;
    }
    {
        Topology topology{};
        cpu(topology, 0, 1024, 1800000);
        cpu(topology, 2, 1024, 2400000);
        const auto result = select(topology, mask({0, 2}));
        require(!result.change && result.reason == Reason::uniformCapacity
                    && result.criterion == Criterion::capacity,
                "complete equal capacity is not overridden by frequency heuristics");
        ++scenarios;
    }
    {
        Topology topology{};
        cpu(topology, 0, 1024, 2000000);
        cpu(topology, 2, 0, 2400000);
        const auto result = select(topology, mask({0, 2}));
        const auto expected = mask({2});
        require(result.change && result.criterion == Criterion::maximumFrequency
                    && CPU_EQUAL(&result.requested, &expected),
                "incomplete capacity falls back to complete maximum-frequency data");
        ++scenarios;
    }
    {
        Topology topology{};
        cpu(topology, 0, 1024, 0);
        cpu(topology, 2, 0, 2400000);
        const auto result = select(topology, mask({0, 2}));
        require(!result.change && result.reason == Reason::incompleteMetadata,
                "partial metrics cannot be combined or missing CPUs discarded");
        ++scenarios;
    }
    {
        Topology topology{};
        cpu(topology, 0, -1, 2400000);
        cpu(topology, 2, 1024, -1);
        const auto result = select(topology, mask({0, 2}));
        require(!result.change && result.reason == Reason::incompleteMetadata,
                "nonpositive metadata is invalid");
        ++scenarios;
    }
    {
        Topology topology{};
        cpu(topology, 0, 0, 2400000);
        cpu(topology, 2, 0, 2400000);
        const auto result = select(topology, mask({0, 2}));
        require(!result.change && result.reason == Reason::uniformFrequency,
                "equal frequency provides no performance subset");
        ++scenarios;
    }
    {
        Topology topology{};
        const auto result = select(topology, mask({}));
        require(!result.change && result.reason == Reason::emptyMask,
                "empty masks cannot be applied");
        ++scenarios;
    }
    {
        Topology topology{};
        cpu(topology, 0, 1024, 2400000);
        const auto result = select(topology, mask({0, 2}));
        require(!result.change && result.reason == Reason::incompleteMetadata,
                "an allowed CPU absent from sysfs prevents unsafe selection");
        ++scenarios;
    }
    {
        static_assert(CPU_SETSIZE > 64, "This test checks masks larger than uint64_t");
        constexpr int high = CPU_SETSIZE - 1;
        Topology topology{};
        cpu(topology, 1, 383, 2000000);
        cpu(topology, high, 1024, 2400000);
        const auto result = select(topology, mask({1, high}));
        require(result.change && CPU_ISSET(high, &result.requested)
                    && !CPU_ISSET(1, &result.requested),
                "CPU IDs above 63 are retained in selection");
        require(maskText(result.requested) == std::to_string(high),
                "CPU IDs above 63 are retained in diagnostics");
        ++scenarios;
    }
    {
        Topology topology{};
        cpu(topology, 4, 1024, 2400000);
        const auto result = select(topology, mask({4}));
        require(!result.change && result.reason == Reason::uniformCapacity,
                "a single allowed CPU needs no affinity restriction");
        require(maskText(mask({0, 1, 2, 4, 6, 7})) == "0-2,4,6-7",
                "diagnostics compact contiguous CPU ranges without dropping IDs");
        ++scenarios;
    }
    std::printf("CPU placement: %d selection scenarios passed\n", scenarios);
}
