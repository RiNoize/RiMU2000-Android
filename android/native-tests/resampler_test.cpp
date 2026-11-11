// SPDX-License-Identifier: BSD-3-Clause
// Host-only signal tests; no firmware, ROMs or captured audio required.
//
// The oracle evaluates the existing float-table, 64-tap sinc independently at
// each exact rational output position. It has no streaming ring, phase bank,
// phase increment or input_needed()/output_available() implementation to share
// with the code under test. Thus accumulated double-position drift in the
// legacy path is not treated as the definition of the desired sample time.
#include "ui/resampler.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kPcmUnit = 1.0 / 32768.0;
constexpr int kHalf = 32;
using Input = std::vector<s16>;
using Output = std::vector<float>;
struct Rates { int in, out; };
std::string currentCase;
int cases = 0;
double largestOracleError = 0.0;

void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(currentCase + ": " + message);
}

std::size_t inputFrames(Rates rates, std::size_t outputs) {
    if (!outputs) return 0;
    if (rates.in == rates.out) return outputs;
    return ((outputs - 1) * std::uint64_t(rates.in)) / rates.out + kHalf + 1;
}

const std::vector<float>& kernelTable() {
    static const std::vector<float> table = [] {
        std::vector<float> result(kHalf * 256 + 2);
        for (std::size_t j = 0; j < result.size(); ++j) {
            const double distance = double(j) / 256.0;
            const double argument = kPi * distance;
            const double sinc = j ? std::sin(argument) / argument : 1.0;
            const double position = (distance + kHalf) / 64.0;
            const double window = 0.42 - 0.5 * std::cos(2.0 * kPi * position)
                                + 0.08 * std::cos(4.0 * kPi * position);
            result[j] = float(sinc * window);
        }
        return result;
    }();
    return table;
}

Output reference(const Input& input, Rates rates, std::size_t outputs) {
    Output result(outputs * 2);
    const auto& table = kernelTable();
    const double cutoff = std::min(1.0, double(rates.out) / rates.in) * 0.955;
    for (std::size_t frame = 0; frame < outputs; ++frame) {
        if (rates.in == rates.out) {
            result[frame * 2] = float(input[frame * 2]) / 32768.0f;
            result[frame * 2 + 1] = float(input[frame * 2 + 1]) / 32768.0f;
            continue;
        }
        const std::uint64_t position = frame * std::uint64_t(rates.in);
        const std::int64_t centre = position / rates.out;
        const double fraction = double(position % rates.out) / rates.out;
        // Wider accumulation avoids making the production accumulation order
        // part of this oracle. The public result remains float32 stereo.
        long double left = 0.0, right = 0.0, normalization = 0.0;
        for (int offset = -31; offset <= 32; ++offset) {
            const std::int64_t index = centre + offset;
            if (index < 0 || std::size_t(index) >= input.size() / 2) continue;
            const double location = std::abs((fraction - offset) * cutoff) * 256.0;
            const std::size_t lower = std::size_t(location);
            if (lower + 1 >= table.size()) continue;
            // Subtraction is deliberately float, matching the specified
            // existing lookup table interpolation rather than a new filter.
            const float difference = table[lower + 1] - table[lower];
            const double weight = table[lower] + difference * (location - lower);
            left += weight * (float(input[std::size_t(index) * 2]) / 32768.0f);
            right += weight * (float(input[std::size_t(index) * 2 + 1]) / 32768.0f);
            normalization += weight;
        }
        if (normalization > 1e-9L) {
            left /= normalization;
            right /= normalization;
        }
        result[frame * 2] = float(std::clamp(left, -1.0L, 1.0L));
        result[frame * 2 + 1] = float(std::clamp(right, -1.0L, 1.0L));
    }
    return result;
}

Output render(ui::resampler& converter, const Input& input, Rates rates,
              std::size_t outputs, const std::vector<int>& chunks) {
    expect(!chunks.empty(), "missing chunk pattern");
    expect(converter.input_needed(0) == 0, "zero output requests require input");
    expect(converter.output_available() == 0, "fresh converter has pending output");
    Output result(outputs * 2);
    std::size_t produced = 0, supplied = 0, chunkIndex = 0;
    while (produced < outputs) {
        const int count = int(std::min<std::size_t>(chunks[chunkIndex++ % chunks.size()],
                                                  outputs - produced));
        expect(count > 0, "nonpositive chunk");
        const int need = converter.input_needed(count);
        expect(need >= 0, "negative input request");
        expect(supplied + std::size_t(need) <= input.size() / 2, "input overread");
        // The caller must not overwrite still-needed samples in the 4096-frame
        // ring. All patterns used here fit, including 44.1k -> 8k downsampling.
        expect(need < 4000, "test chunk would overrun converter history");
        if (converter.phase_count() || converter.direct())
            expect(supplied + need == inputFrames(rates, produced + count),
                   "input request disagrees with exact absolute sample position");
        converter.push(input.data() + supplied * 2, need);
        supplied += need;
        expect(std::uint64_t(converter.written()) == supplied, "wrong input counter");
        if (converter.phase_count() || converter.direct())
            expect(converter.output_available() >= count, "requested output is not available");
        converter.pull(result.data() + produced * 2, count);
        produced += count;
        if (converter.phase_count()) {
            // Independently count integer output times whose final tap exists.
            // This also covers the fractional endpoint the legacy API omitted.
            const std::int64_t numerator = (std::int64_t(supplied) - kHalf) * rates.out
                                        - 1 - std::int64_t(produced) * rates.in;
            const std::int64_t available = numerator < 0 ? 0 : numerator / rates.in + 1;
            expect(converter.output_available() == available, "wrong exact output availability");
        }
    }
    return result;
}

int pcm(float value) {
    return int(std::clamp(std::lround(double(value) * 32768.0), -32768L, 32767L));
}

double compare(const Output& actual, const Output& expected, double tolerance,
               int pcmTolerance) {
    expect(actual.size() == expected.size(), "different output lengths");
    double maximum = 0;
    int pcmMaximum = 0;
    for (std::size_t i = 0; i < actual.size(); ++i) {
        expect(std::isfinite(actual[i]) && actual[i] >= -1.0f && actual[i] <= 1.0f,
               "nonfinite or unclamped output");
        maximum = std::max(maximum, std::abs(double(actual[i]) - expected[i]));
        pcmMaximum = std::max(pcmMaximum, std::abs(pcm(actual[i]) - pcm(expected[i])));
    }
    if (maximum > tolerance || pcmMaximum > pcmTolerance) {
        char detail[192];
        std::snprintf(detail, sizeof(detail), "maximum error %.9g (%.5f PCM LSB), PCM difference %d",
                      maximum, maximum * 32768.0, pcmMaximum);
        throw std::runtime_error(currentCase + ": " + detail);
    }
    return maximum;
}

enum class Signal { silence, dc, impulses, fullscale, random, sweep };
const char* signalName(Signal signal) {
    switch (signal) {
    case Signal::silence: return "silence";
    case Signal::dc: return "DC";
    case Signal::impulses: return "impulses";
    case Signal::fullscale: return "fullscale";
    case Signal::random: return "random";
    default: return "sweep";
    }
}

Input makeInput(Signal signal, Rates rates, std::size_t outputs) {
    const std::size_t frames = inputFrames(rates, outputs) + 64;
    Input result(frames * 2);
    std::uint32_t random = 0x9d14abd7U;
    const auto next = [&random] {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        return s16(int(random & 0xffffU) - 32768);
    };
    for (std::size_t i = 0; i < frames; ++i) {
        switch (signal) {
        case Signal::silence: break;
        case Signal::dc:
            result[i * 2] = 12345;
            result[i * 2 + 1] = -32768;
            break;
        case Signal::impulses:
            if (i == 0 || i == 31 || i == 257 || i == 4095 || i == 4096 || i == 8011)
                result[i * 2] = (i & 1) ? -32768 : 32767;
            // The other channel remains silent to catch channel mixing.
            break;
        case Signal::fullscale:
            result[i * 2] = (i & 1) ? -32768 : 32767;
            result[i * 2 + 1] = ((i / 37) & 1) ? -32768 : 32767;
            break;
        case Signal::random:
            result[i * 2] = next();
            result[i * 2 + 1] = next();
            break;
        case Signal::sweep: {
            const double t = double(i) / rates.in;
            const double duration = double(frames) / rates.in;
            const double angle = 2 * kPi * (20 * t + (0.49 * rates.in - 20) * t * t / (2 * duration));
            result[i * 2] = s16(std::lround(30000 * std::sin(angle)));
            result[i * 2 + 1] = s16(std::lround(17000 * std::cos(angle)));
            break;
        }
        }
    }
    return result;
}

void signalCases() {
    const std::array<Rates, 10> rates{{{44100, 48000}, {44100, 96000}, {44100, 192000},
        {44100, 8000}, {44100, 16000}, {44100, 22050}, {44100, 32000},
        {48000, 44100}, {44100, 8192}, {44100, 44100}}};
    const std::array<Signal, 6> signals{{Signal::silence, Signal::dc, Signal::impulses,
        Signal::fullscale, Signal::random, Signal::sweep}};
    constexpr std::size_t outputs = 8193;
    for (const auto rate : rates) {
        for (const auto signal : signals) {
            currentCase = std::to_string(rate.in) + " -> " + std::to_string(rate.out) + " " + signalName(signal);
            const auto input = makeInput(signal, rate, outputs);
            ui::resampler converter;
            converter.configure_precomputed_sinc(rate.in, rate.out);
            const int phases = rate.in == rate.out ? 0 : rate.out / std::gcd(rate.in, rate.out);
            expect(converter.phase_count() == phases, "wrong phase-bank size");
            const auto actual = render(converter, input, rate, outputs, {256});
            const auto expected = reference(input, rate, outputs);
            // 1e-6 normalized amplitude is 0.032768 PCM16 LSB; this is not a
            // tolerance that could hide a different interpolation/filter mode.
            largestOracleError = std::max(largestOracleError, compare(actual, expected, 1e-6, 1));
            if (signal == Signal::silence || signal == Signal::dc || signal == Signal::impulses) {
                for (std::size_t i = 0; i < outputs; ++i) {
                    if (signal == Signal::silence)
                        expect(actual[i * 2] == 0.0f && actual[i * 2 + 1] == 0.0f, "silence is not exact");
                    if (signal == Signal::impulses)
                        expect(actual[i * 2 + 1] == 0.0f, "channel crosstalk");
                    if (signal == Signal::dc)
                        expect(std::abs(actual[i * 2] - 12345.0f / 32768.0f) <= 1e-7f
                               && actual[i * 2 + 1] == -1.0f, "DC not preserved, including startup");
                }
            }
            converter.reset();
            const auto rechunked = render(converter, input, rate, outputs, {1, 17, 511, 3, 127, 256});
            expect(std::memcmp(actual.data(), rechunked.data(), actual.size() * sizeof(float)) == 0,
                   "reset or chunk boundaries changed output bits");
            ++cases;
        }
    }
}

void configurationCases() {
    ui::resampler converter;
    currentCase = "phase cap, fallback and reconfiguration";
    converter.configure_precomputed_sinc(44100, 8192);
    expect(converter.phase_count() == 2048, "phase-cap boundary rejected");
    for (const auto rate : {Rates{44100, 8193}, Rates{44100, 47999}}) {
        converter.configure_precomputed_sinc(rate.in, rate.out);
        expect(converter.phase_count() == 0, "oversized phase-bank did not fall back");
        const auto input = makeInput(Signal::random, rate, 4097);
        const auto actual = render(converter, input, rate, 4097, {127});
        ui::resampler legacy;
        legacy.configure(rate.in, rate.out);
        const auto expected = render(legacy, input, rate, 4097, {127});
        expect(std::memcmp(actual.data(), expected.data(), actual.size() * sizeof(float)) == 0,
               "fallback differs from the unchanged legacy sinc");
    }
    for (const auto quality : {ui::resampler_quality::sinc, ui::resampler_quality::linear,
                               ui::resampler_quality::nearest}) {
        converter.configure_precomputed_sinc(44100, 48000);
        converter.configure(44100, 48000, quality);
        expect(converter.phase_count() == 0, "desktop configure retained cached phases");
        const auto input = makeInput(Signal::random, {44100, 48000}, 4097);
        const auto actual = render(converter, input, {44100, 48000}, 4097, {127});
        ui::resampler fresh;
        fresh.configure(44100, 48000, quality);
        const auto expected = render(fresh, input, {44100, 48000}, 4097, {127});
        expect(std::memcmp(actual.data(), expected.data(), actual.size() * sizeof(float)) == 0,
               "legacy quality changed after cached configuration");
    }
    converter.configure_precomputed_sinc(0, 48000);
    expect(converter.phase_count() == 160, "invalid input-rate fallback changed");
    converter.configure_precomputed_sinc(44100, 0);
    expect(converter.phase_count() == 0 && converter.direct(), "invalid output-rate fallback changed");
    ++cases;
}

double sineRms(int frequency) {
    const Rates rate{44100, 32000};
    constexpr std::size_t skip = 320, measured = 32000, outputs = skip + measured;
    Input input((inputFrames(rate, outputs) + 64) * 2);
    for (std::size_t i = 0; i < input.size() / 2; ++i)
        input[i * 2] = s16(std::lround(16000 * std::sin(2 * kPi * frequency * double(i) / rate.in)));
    ui::resampler converter;
    converter.configure_precomputed_sinc(rate.in, rate.out);
    const auto output = render(converter, input, rate, outputs, {256});
    double energy = 0;
    for (std::size_t i = skip; i < outputs; ++i) energy += double(output[i * 2]) * output[i * 2];
    return std::sqrt(energy / measured);
}

void frequencyCase() {
    currentCase = "32k downsampling passband/alias";
    const double pass = sineRms(1000);
    const double stop = sineRms(19000); // above output Nyquist; folds to 13k without filtering
    const double expected = (16000.0 / 32768.0) / std::sqrt(2.0);
    expect(std::abs(pass / expected - 1.0) < 0.01, "1kHz passband amplitude changed by over 1 percent");
    expect(stop / pass < 0.003, "19kHz alias is not attenuated by at least 50.4dB");
    std::printf("Downsampling 44.1k -> 32k: 1k gain %.6f, 19k relative level %.2f dB\n",
                pass / expected, 20 * std::log10(stop / pass));
    ++cases;
}

void longTimingCase() {
    currentCase = "long rational timeline";
    const Rates rate{44100, 48000};
    constexpr std::size_t outputs = 160 * 8192 + 1;
    const auto input = makeInput(Signal::silence, rate, outputs);
    ui::resampler converter;
    converter.configure_precomputed_sinc(rate.in, rate.out);
    const auto result = render(converter, input, rate, outputs, {511, 1, 127, 256});
    expect(std::all_of(result.begin(), result.end(), [](float x) { return x == 0; }), "long silence changed");
    expect(std::uint64_t(converter.written()) == inputFrames(rate, outputs), "rational timeline drifted");
    ++cases;
}

void legacyComparisonCase() {
    currentCase = "48k comparison with legacy accumulated double position";
    const Rates rate{44100, 48000};
    constexpr std::size_t outputs = 48000;
    const auto input = makeInput(Signal::random, rate, outputs);
    ui::resampler cached, legacy;
    cached.configure_precomputed_sinc(rate.in, rate.out);
    legacy.configure(rate.in, rate.out);
    const auto actual = render(cached, input, rate, outputs, {256});
    const auto expected = render(legacy, input, rate, outputs, {256});
    // An almost-integer legacy position can select the adjacent 64-tap window.
    // At 44.1k -> 48k the nonzero end tap permits ~1.2 PCM LSB worst-case change.
    // This is intentionally a separate, looser comparison from the oracle.
    const double maximum = compare(actual, expected, 1.5 * kPcmUnit, 2);
    std::printf("48k legacy comparison: maximum %.6f PCM16 LSB (timing is now rational)\n",
                maximum / kPcmUnit);
    ++cases;
}
} // namespace

int main() {
    try {
        signalCases();
        configurationCases();
        frequencyCase();
        longTimingCase();
        legacyComparisonCase();
        std::printf("resampler tests passed: %d cases; maximum rational-oracle error %.9g (%.6f PCM16 LSB)\n",
                    cases, largestOracleError, largestOracleError / kPcmUnit);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "resampler test failed: %s\n", error.what());
        return 1;
    }
}
