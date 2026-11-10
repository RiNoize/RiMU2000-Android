// SPDX-License-Identifier: BSD-3-Clause
// Android owns devices and lifecycle; the upstream MU2000 owns synthesis.
#include <aaudio/AAudio.h>
#include <jni.h>

#include "mu2000.h"
#include "ui/resampler.h"
#include "midi_queue.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <time.h>
#include <vector>

namespace {
constexpr int kEngineRate = 44100;
constexpr int kRenderChunk = 256;
constexpr int kStagingFrames = 2048;

std::uint64_t clockNs(clockid_t clock) {
    timespec ts{};
    if (clock_gettime(clock, &ts) != 0) return 0;
    return std::uint64_t(ts.tv_sec) * 1000000000ULL + std::uint64_t(ts.tv_nsec);
}

std::string fromJava(JNIEnv* env, jstring text) {
    if (!text) return {};
    const char* chars = env->GetStringUTFChars(text, nullptr);
    if (!chars) return {};
    std::string result(chars);
    env->ReleaseStringUTFChars(text, chars);
    return result;
}

enum class State { stopped, booting, playing, disconnected, failed };

class Engine {
public:
    std::string start(const std::string& romDir, const std::string& stateDir,
                      int frames, int deviceId) {
        std::lock_guard<std::mutex> guard(control_);
        stopLocked();
        cancelled_.store(false, std::memory_order_release);
        state_.store(State::booting, std::memory_order_release);
        resetStatistics();
        try {
            frames = (frames == 512 || frames == 1024 || frames == 2048) ? frames : 1024;
            requestedFrames_.store(frames);
            stateDir_ = stateDir;
            mu_ = std::make_unique<mu2000>();
            if (!mu_->load_program(romDir + "/mu2000_flash.bin"))
                return fail("No se pudo cargar el firmware: " + mu_->error());
            if (!mu_->load_wave(romDir + "/dump"))
                return fail("No se pudieron cargar las ondas: " + mu_->error());

            // This is upstream's mathematical stand-in, not hardware data.
            // Importing a valid sine table overrides it.
            if (!mu_->load_sintab(romDir + "/standin/sin-table.bin")) {
                auto sine = std::make_shared<std::vector<u16>>(0x8000);
                for (std::size_t i = 0; i < sine->size(); ++i)
                    (*sine)[i] = u16(std::min(65535.0, std::round(0x8000 +
                        std::sin((i + 0.5) / 0x8000 * 3.14159265358979323846 / 2) * 0x7fff)));
                mu_->set_sintab_rom(std::move(sine));
            }
            // Keep firmware, SH2 JIT, both SWP30s and MEG JIT. The upstream
            // parallel slave synchronizes per sample; measure one render
            // thread first on Android, avoiding priority inversion.
            mu_->set_threaded(false);
            mu_->set_usb_host(true);
            mu_->set_fast_midi(true);
            loadNvram();
            mu_->reset();

            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            s32 left = 0, right = 0;
            int generated = 0;
            for (; generated < 30 * kEngineRate && !mu_->midi_ready(); ++generated) {
                if ((generated & 255) == 0) {
                    if (cancelled_.load(std::memory_order_acquire))
                        return fail("Inicio cancelado.", State::stopped);
                    if (std::chrono::steady_clock::now() >= deadline)
                        return fail("El firmware no terminó de iniciar en 30 segundos. Revisá las ROMs.");
                }
                mu_->run_sample(left, right);
            }
            if (!mu_->midi_ready())
                return fail("El firmware no terminó de iniciar. Revisá las ROMs.");
            booted_ = true;
            for (int i = 0; i < kEngineRate / 20; ++i) mu_->run_sample(left, right);
            if (cancelled_.load(std::memory_order_acquire))
                return fail("Inicio cancelado.", State::stopped);

            AAudioStreamBuilder* builder = nullptr;
            aaudio_result_t result = AAudio_createStreamBuilder(&builder);
            if (result != AAUDIO_OK) return fail(audioError("Crear audio", result));
            AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
            AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
            AAudioStreamBuilder_setChannelCount(builder, 2);
            AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
            AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
            // Let the device choose its real rate; the MU2000 always runs at 44.1 kHz.
            AAudioStreamBuilder_setSampleRate(builder, AAUDIO_UNSPECIFIED);
            AAudioStreamBuilder_setFramesPerDataCallback(builder, frames);
            AAudioStreamBuilder_setBufferCapacityInFrames(builder, std::max(4096, frames * 2));
            if (deviceId > 0) AAudioStreamBuilder_setDeviceId(builder, deviceId);
            AAudioStreamBuilder_setDataCallback(builder, &Engine::dataCallback, this);
            AAudioStreamBuilder_setErrorCallback(builder, &Engine::errorCallback, this);
            result = AAudioStreamBuilder_openStream(builder, &stream_);
            if (result == AAUDIO_ERROR_OUT_OF_RANGE) {
                // MMAP devices can cap their buffer below our requested size.
                // Let AAudio choose callback frames while keeping the requested
                // buffering target. Diagnostics show the size actually received.
                AAudioStreamBuilder_setFramesPerDataCallback(builder, AAUDIO_UNSPECIFIED);
                result = AAudioStreamBuilder_openStream(builder, &stream_);
            }
            AAudioStreamBuilder_delete(builder);
            if (result != AAUDIO_OK) return fail(audioError("Abrir salida", result));
            const int rate = AAudioStream_getSampleRate(stream_);
            if (AAudioStream_getFormat(stream_) != AAUDIO_FORMAT_PCM_FLOAT ||
                AAudioStream_getChannelCount(stream_) != 2 || rate < 8000 || rate > 384000)
                return fail("La salida no ofrece el formato estéreo requerido.");
            rate_.store(rate);
            observedFrames_.store(AAudioStream_getFramesPerDataCallback(stream_));
            device_.store(AAudioStream_getDeviceId(stream_));
            const int burst = std::max(1, AAudioStream_getFramesPerBurst(stream_));
            burst_.store(burst);
            const int capacity = AAudioStream_getBufferCapacityInFrames(stream_);
            const int wanted = ((std::max(frames, 2 * burst) + burst - 1) / burst) * burst;
            result = AAudioStream_setBufferSizeInFrames(stream_, std::min(capacity, wanted));
            if (result < 0) return fail(audioError("Ajustar buffer", result));
            actualBuffer_.store(AAudioStream_getBufferSizeInFrames(stream_));
            resampler_.configure(kEngineRate, rate, ui::resampler_quality::sinc);
            midi_.reset();
            panic_.store(false);
            midi_.setEnabled(true);
            startTime_.store(clockNs(CLOCK_MONOTONIC));
            startCpu_.store(clockNs(CLOCK_PROCESS_CPUTIME_ID));
            running_.store(true, std::memory_order_release);
            state_.store(State::playing, std::memory_order_release);
            result = AAudioStream_requestStart(stream_);
            if (result != AAUDIO_OK) return fail(audioError("Iniciar salida", result));
            if (cancelled_.load(std::memory_order_acquire))
                return fail("Inicio cancelado.", State::stopped);
            return {};
        } catch (const std::exception& e) {
            return fail(std::string("No se pudo iniciar: ") + e.what());
        } catch (...) {
            return fail("No se pudo iniciar el motor de audio.");
        }
    }

    void requestStop() {
        cancelled_.store(true, std::memory_order_release);
        midi_.setEnabled(false);
    }

    void stop() {
        requestStop();
        std::lock_guard<std::mutex> guard(control_);
        stopLocked();
        state_.store(State::stopped, std::memory_order_release);
    }

    void midi(const std::uint8_t* bytes, std::size_t size, int port) {
        if (!running_.load(std::memory_order_acquire) || cancelled_.load()) return;
        if (!midi_.enqueue(bytes, size, std::uint8_t(std::clamp(port, 0, 3)))) {
            dropped_.fetch_add(size, std::memory_order_relaxed);
            panic_.store(true, std::memory_order_release);
        }
    }

    void panic() { panic_.store(true, std::memory_order_release); }
    void rejectMidi(std::size_t size) {
        if (!running()) return;
        dropped_.fetch_add(size, std::memory_order_relaxed);
        panic();
    }
    bool running() const { return running_.load(std::memory_order_acquire); }
    void gain(float value) {
        if (std::isfinite(value)) gain_.store(std::clamp(value, 0.0f, 1.0f));
    }

    // No engine/stream access or control lock here: safe on the UI thread even
    // while start is loading ROMs or stop is joining the callback.
    std::string stats() const {
        const auto state = state_.load(std::memory_order_acquire);
        if (state == State::booting) return "Iniciando firmware del MU2000…";
        const char* label = state == State::playing ? "Audio activo" :
                            state == State::disconnected ? "Salida desconectada. Volvé a iniciar." :
                            state == State::failed ? "Audio detenido por un error" : "Audio detenido";
        const auto samples = frames_.load();
        const auto elapsed = elapsedNs_.load();
        const int rate = rate_.load();
        const double average = samples && rate > 0 ?
            double(elapsed) * rate / (double(samples) * 10000000.0) : 0.0;
        const auto time = clockNs(CLOCK_MONOTONIC);
        const auto cpu = clockNs(CLOCK_PROCESS_CPUTIME_ID);
        const auto began = startTime_.load(), cpuBegan = startCpu_.load();
        const double appCpu = running() && began && time > began && cpu >= cpuBegan ?
            double(cpu - cpuBegan) * 100.0 / double(time - began) : 0.0;
        char out[1024];
        std::snprintf(out, sizeof(out),
            "%s\nRender medio %.1f%% · máximo %.1f%% · CPU app %.1f%%\n"
            "Xruns %d · callbacks fuera de presupuesto %llu / %llu\n"
            "44100 → %d Hz · callback %d fr (pedido %d) · buffer real %d fr · ráfaga %d\n"
            "MIDI %llu bytes · descartados %llu · pendientes %llu · salida ID %d\n"
            "ARM64 · JIT SH2/MEG compilados · firmware original · 1 hilo de render%s",
            label, average, double(maxPercent_.load()), appCpu, xruns_.load(),
            static_cast<unsigned long long>(late_.load()),
            static_cast<unsigned long long>(callbacks_.load()), rate, observedFrames_.load(), requestedFrames_.load(),
            actualBuffer_.load(), burst_.load(),
            static_cast<unsigned long long>(midiReceived_.load()),
            static_cast<unsigned long long>(dropped_.load() + internalDropped_.load()),
            static_cast<unsigned long long>(pendingMidi_.load()), device_.load(),
            nvramWarning_.load() ? "\nNo se pudieron guardar los ajustes de la sesión." : "");
        return out;
    }

private:
    static std::string audioError(const char* operation, aaudio_result_t result) {
        return std::string(operation) + ": " + AAudio_convertResultToText(result);
    }

    void loadNvram() {
        std::FILE* file = std::fopen((stateDir_ + "/nvram.bin").c_str(), "rb");
        if (!file) return;
        std::array<u8, 0x40000> data{};
        const auto read = std::fread(data.data(), 1, data.size(), file);
        const bool exact = read == data.size() && std::fgetc(file) == EOF && !std::ferror(file);
        std::fclose(file);
        if (exact) mu_->set_nvram(data.data(), data.size());
    }

    void saveNvram() noexcept {
        if (!mu_ || !booted_ || stateDir_.empty()) return;
        try {
            std::error_code ec;
            std::filesystem::create_directories(stateDir_, ec);
            if (ec) { nvramWarning_.store(true); return; }
            const auto path = stateDir_ + "/nvram.bin";
            const auto temp = path + ".tmp";
            std::FILE* file = std::fopen(temp.c_str(), "wb");
            if (!file) { nvramWarning_.store(true); return; }
            const auto& data = mu_->nvram();
            bool ok = std::fwrite(data.data(), 1, data.size(), file) == data.size();
            if (std::fflush(file) != 0) ok = false;
            if (std::fclose(file) != 0) ok = false;
            if (ok && std::rename(temp.c_str(), path.c_str()) == 0) return;
            std::remove(temp.c_str());
            nvramWarning_.store(true);
        } catch (...) { nvramWarning_.store(true); }
    }

    void stopLocked() {
        running_.store(false, std::memory_order_release);
        midi_.setEnabled(false);
        if (stream_) {
            AAudioStream_requestStop(stream_);
            aaudio_stream_state_t state = AAudioStream_getState(stream_);
            for (int attempt = 0; attempt < 20 &&
                 (state == AAUDIO_STREAM_STATE_STARTING || state == AAUDIO_STREAM_STATE_STARTED ||
                  state == AAUDIO_STREAM_STATE_STOPPING); ++attempt) {
                aaudio_stream_state_t next = state;
                AAudioStream_waitForStateChange(stream_, state, &next, 100000000);
                state = next;
            }
            AAudioStream_close(stream_);
            stream_ = nullptr;
        }
        // AAudio close has joined its callback before the emulator is touched.
        saveNvram();
        mu_.reset();
        booted_ = false;
        midi_.reset();
    }

    std::string fail(std::string message, State state = State::failed) {
        stopLocked();
        state_.store(state, std::memory_order_release);
        return message;
    }

    void resetStatistics() {
        frames_.store(0); elapsedNs_.store(0); callbacks_.store(0); late_.store(0);
        maxPercent_.store(0); xruns_.store(0); dropped_.store(0); midiReceived_.store(0);
        internalDropped_.store(0); pendingMidi_.store(0); observedFrames_.store(0);
        rate_.store(0); actualBuffer_.store(0); burst_.store(0); device_.store(0);
        startTime_.store(0); startCpu_.store(0); nvramWarning_.store(false);
    }

    void sendPanic() {
        midi_.discard();
        // A stopped/truncated SysEx must not swallow the channel messages.
        for (int port = 0; port < 4; ++port) {
            mu_->midi_in(0xf7, port);
            for (int channel = 0; channel < 16; ++channel) {
                for (int cc : {64, 123, 120}) {
                    mu_->midi_in(u8(0xb0 | channel), port);
                    mu_->midi_in(u8(cc), port);
                    mu_->midi_in(0, port);
                }
            }
        }
    }

    static aaudio_data_callback_result_t dataCallback(
            AAudioStream* stream, void* context, void* audio, int32_t count) {
        return static_cast<Engine*>(context)->render(stream, static_cast<float*>(audio), count);
    }

    aaudio_data_callback_result_t render(AAudioStream* stream, float* output, int count) {
        if (count <= 0) return AAUDIO_CALLBACK_RESULT_CONTINUE;
        if (!running_.load(std::memory_order_acquire) || cancelled_.load(std::memory_order_acquire)) {
            std::fill_n(output, std::size_t(count) * 2, 0.0f);
            return AAUDIO_CALLBACK_RESULT_STOP;
        }
        const auto begin = clockNs(CLOCK_MONOTONIC);
        try {
            for (int done = 0; done < count; ) {
                if (panic_.exchange(false, std::memory_order_acq_rel)) sendPanic();
                // Keep the firmware's queue bounded rather than merely moving a
                // MIDI flood from one queue to another. This also bounds how much
                // work precedes a panic message inside the firmware.
                const auto pending = mu_->midi_pending();
                const std::size_t allowance = pending < 2048 ? 2048 - pending : 0;
                const auto received = midi_.consume(allowance, [&](u8 byte, u8 port) { mu_->midi_in(byte, port); });
                midiReceived_.fetch_add(received, std::memory_order_relaxed);
                const int chunk = std::min(kRenderChunk, count - done);
                const int need = resampler_.input_needed(chunk);
                if (need < 0 || need > kStagingFrames) throw std::runtime_error("Resampler capacity");
                for (int i = 0; i < need; ++i) {
                    s32 l = 0, r = 0;
                    mu_->run_sample(l, r);
                    // Match upstream live's 16-bit DAC conversion before sinc.
                    staging_[i * 2] = s16(std::clamp(l >> 2, -32768, 32767));
                    staging_[i * 2 + 1] = s16(std::clamp(r >> 2, -32768, 32767));
                }
                resampler_.push(staging_.data(), need);
                resampler_.pull(output + done * 2, chunk);
                const float gain = gain_.load(std::memory_order_relaxed);
                for (int i = 0; i < chunk * 2; ++i) output[done * 2 + i] *= gain;
                done += chunk;
            }
        } catch (...) {
            std::fill_n(output, std::size_t(count) * 2, 0.0f);
            running_.store(false);
            state_.store(State::failed);
            return AAUDIO_CALLBACK_RESULT_STOP;
        }
        const auto elapsed = clockNs(CLOCK_MONOTONIC) - begin;
        const double fraction = double(elapsed) * rate_.load(std::memory_order_relaxed) /
                                (double(count) * 1000000000.0);
        const float percent = float(fraction * 100.0);
        // Only the callback writes maxima while playing; reset is after close.
        if (percent > maxPercent_.load(std::memory_order_relaxed)) maxPercent_.store(percent);
        if (fraction > 1.0) late_.fetch_add(1, std::memory_order_relaxed);
        elapsedNs_.fetch_add(elapsed, std::memory_order_relaxed);
        observedFrames_.store(count, std::memory_order_relaxed);
        internalDropped_.store(mu_->midi_dropped(), std::memory_order_relaxed);
        pendingMidi_.store(mu_->midi_pending(), std::memory_order_relaxed);
        frames_.fetch_add(std::uint64_t(count), std::memory_order_relaxed);
        callbacks_.fetch_add(1, std::memory_order_relaxed);
        xruns_.store(std::max(0, AAudioStream_getXRunCount(stream)), std::memory_order_relaxed);
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }

    static void errorCallback(AAudioStream*, void* context, aaudio_result_t) {
        auto& self = *static_cast<Engine*>(context);
        self.midi_.setEnabled(false);
        self.running_.store(false, std::memory_order_release);
        self.state_.store(State::disconnected, std::memory_order_release);
        // Closing here would deadlock AAudio. The Java control worker closes it.
    }

    std::mutex control_;
    std::unique_ptr<mu2000> mu_;
    AAudioStream* stream_ = nullptr;
    ui::resampler resampler_;
    std::array<s16, kStagingFrames * 2> staging_{};
    rimu2000::MidiQueue midi_;
    std::string stateDir_;
    bool booted_ = false;
    std::atomic<bool> running_{false}, cancelled_{false}, panic_{false}, nvramWarning_{false};
    std::atomic<State> state_{State::stopped};
    std::atomic<float> gain_{0.8f}, maxPercent_{0};
    std::atomic<int> rate_{0}, requestedFrames_{1024}, observedFrames_{0}, actualBuffer_{0}, burst_{0}, device_{0}, xruns_{0};
    std::atomic<std::uint64_t> frames_{0}, elapsedNs_{0}, callbacks_{0}, late_{0}, dropped_{0},
        midiReceived_{0}, internalDropped_{0}, pendingMidi_{0}, startTime_{0}, startCpu_{0};
};

// Intentionally process-lifetime: close explicitly on the lifecycle worker.
// No C++ static destructor may race the Android audio service during exit.
Engine& engine() { static Engine* instance = new Engine; return *instance; }
} // namespace

extern "C" JNIEXPORT jstring JNICALL
Java_com_rinoize_rimu2000_NativeEngine_start(JNIEnv* env, jclass, jstring roms,
                                           jstring state, jint frames, jint device) {
    const auto result = engine().start(fromJava(env, roms), fromJava(env, state), frames, device);
    return env->NewStringUTF(result.c_str());
}

extern "C" JNIEXPORT void JNICALL
Java_com_rinoize_rimu2000_NativeEngine_stop(JNIEnv*, jclass) { engine().stop(); }

extern "C" JNIEXPORT void JNICALL
Java_com_rinoize_rimu2000_NativeEngine_requestStop(JNIEnv*, jclass) { engine().requestStop(); }

extern "C" JNIEXPORT void JNICALL
Java_com_rinoize_rimu2000_NativeEngine_panic(JNIEnv*, jclass) { engine().panic(); }

extern "C" JNIEXPORT void JNICALL
Java_com_rinoize_rimu2000_NativeEngine_setGain(JNIEnv*, jclass, jfloat value) { engine().gain(value); }

extern "C" JNIEXPORT jboolean JNICALL
Java_com_rinoize_rimu2000_NativeEngine_isRunning(JNIEnv*, jclass) {
    return engine().running() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_rinoize_rimu2000_NativeEngine_stats(JNIEnv* env, jclass) {
    const auto text = engine().stats();
    return env->NewStringUTF(text.c_str());
}

extern "C" JNIEXPORT void JNICALL
Java_com_rinoize_rimu2000_NativeEngine_sendMidi(JNIEnv* env, jclass, jbyteArray data,
                                              jint offset, jint count, jint port) {
    if (!data || offset < 0 || count <= 0 || port < 0 || port > 3) return;
    const auto size = env->GetArrayLength(data);
    if (offset > size || count > size - offset) return;
    // Allocation is on the Java MIDI/UI caller, never the audio thread.
    // Very large chunks are rejected together so MIDI framing stays intact.
    if (std::size_t(count) > rimu2000::MidiQueue::capacity) { engine().rejectMidi(count); return; }
    try {
        std::vector<u8> bytes(static_cast<std::size_t>(count));
        env->GetByteArrayRegion(data, offset, count, reinterpret_cast<jbyte*>(bytes.data()));
        if (!env->ExceptionCheck()) engine().midi(bytes.data(), bytes.size(), port);
    } catch (...) { engine().panic(); }
}
