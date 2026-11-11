// SPDX-License-Identifier: BSD-3-Clause
// Android owns devices and lifecycle; the upstream MU2000 owns synthesis.
#include <aaudio/AAudio.h>
#include <jni.h>

#include "mu2000.h"
#include "ui/resampler.h"
#include "midi_queue.h"
#include "performance_hint.h"
#include "audio_cpu_placement.h"
#include "patch_snapshot.h"
#include "program_change_tracker.h"
#include "callback_diagnostics.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <sched.h>
#include <stdexcept>
#include <string>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>
#include <vector>

namespace {
constexpr int kEngineRate = 44100;
constexpr int kRenderChunk = 256;
constexpr int kStagingFrames = 2048;
constexpr std::uint64_t kStatisticsWindowNs = 1000000000ULL;
constexpr std::uint64_t kPatchWindowNs = 200000000ULL;

std::uint64_t clockNs(clockid_t clock) {
    timespec ts{};
    if (clock_gettime(clock, &ts) != 0) return 0;
    return std::uint64_t(ts.tv_sec) * 1000000000ULL + std::uint64_t(ts.tv_nsec);
}

std::uint64_t elapsedSince(std::uint64_t begin, std::uint64_t end) {
    return end >= begin ? end - begin : 0;
}

const char* schedulerName(int policy) {
    if (policy < 0) return "no disponible";
    switch (policy & ~SCHED_RESET_ON_FORK) {
    case SCHED_OTHER: return "NORMAL";
    case SCHED_FIFO: return "FIFO";
    case SCHED_RR: return "RR";
    case SCHED_BATCH: return "BATCH";
    case SCHED_IDLE: return "IDLE";
    default: return "?";
    }
}

struct TimingWindow {
    std::uint64_t started = 0, span = 0, frames = 0, elapsed = 0, cpu = 0;
    std::uint64_t core = 0, conversion = 0, callbacks = 0, late = 0, cpuMask = 0;
    float maximum = 0;
};

// One writer (audio), non-blocking readers (UI). Every data member is atomic,
// including across the brief publication boundary; readers retry at most twice.
struct PublishedWindow {
    std::atomic<std::uint64_t> serial{0}, span{0}, frames{0}, elapsed{0}, cpu{0},
        core{0}, conversion{0}, callbacks{0}, late{0}, cpuMask{0};
    std::atomic<float> maximum{0};

    void publish(const TimingWindow& window) {
        serial.fetch_add(1, std::memory_order_acq_rel);
        span.store(window.span, std::memory_order_relaxed);
        frames.store(window.frames, std::memory_order_relaxed);
        elapsed.store(window.elapsed, std::memory_order_relaxed);
        cpu.store(window.cpu, std::memory_order_relaxed);
        core.store(window.core, std::memory_order_relaxed);
        conversion.store(window.conversion, std::memory_order_relaxed);
        callbacks.store(window.callbacks, std::memory_order_relaxed);
        late.store(window.late, std::memory_order_relaxed);
        cpuMask.store(window.cpuMask, std::memory_order_relaxed);
        maximum.store(window.maximum, std::memory_order_relaxed);
        serial.fetch_add(1, std::memory_order_release);
    }
    bool read(TimingWindow& result) const {
        for (int attempt = 0; attempt < 2; ++attempt) {
            const auto before = serial.load(std::memory_order_acquire);
            if (before & 1) continue;
            result.span = span.load(std::memory_order_relaxed);
            result.frames = frames.load(std::memory_order_relaxed);
            result.elapsed = elapsed.load(std::memory_order_relaxed);
            result.cpu = cpu.load(std::memory_order_relaxed);
            result.core = core.load(std::memory_order_relaxed);
            result.conversion = conversion.load(std::memory_order_relaxed);
            result.callbacks = callbacks.load(std::memory_order_relaxed);
            result.late = late.load(std::memory_order_relaxed);
            result.cpuMask = cpuMask.load(std::memory_order_relaxed);
            result.maximum = maximum.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (before == serial.load(std::memory_order_relaxed))
                return result.callbacks > 0;
        }
        return false;
    }
};

struct PublishedJit {
    // flags: enabled, native SH2 code, ready, ready baked MEG code.
    std::atomic<unsigned> flags{0};
    std::atomic<std::uint64_t> work{0}, jit{0}, interpreter{0}, idle{0}, nativeFx{0},
        baked{0}, failures{0}, bakedFailures{0}, allocationFailures{0},
        compilations{0}, bakedCompilations{0};
    std::atomic<int> error{0};
    void reset() {
        flags.store(0); work.store(0); jit.store(0); interpreter.store(0); idle.store(0);
        nativeFx.store(0); baked.store(0); failures.store(0); bakedFailures.store(0);
        allocationFailures.store(0); error.store(0);
        compilations.store(0); bakedCompilations.store(0);
    }
};

std::string percentage(std::uint64_t part, std::uint64_t whole) {
    if (!whole) return "--";
    char value[32];
    std::snprintf(value, sizeof(value), "%.1f%%", double(part) * 100.0 / double(whole));
    return value;
}

std::string megDescription(const PublishedJit& stats, const char* name) {
    const unsigned flags = stats.flags.load(std::memory_order_relaxed);
    const auto jit = stats.jit.load(std::memory_order_relaxed);
    const auto interpreter = stats.interpreter.load(std::memory_order_relaxed);
    const auto work = stats.work.load(std::memory_order_relaxed);
    const auto idle = stats.idle.load(std::memory_order_relaxed);
    const auto native = stats.nativeFx.load(std::memory_order_relaxed);
    const auto baked = stats.baked.load(std::memory_order_relaxed);
    char out[384];
    std::snprintf(out, sizeof(out),
        "MEG %s: JIT %s · intérp %s · reposo %s · bake %s · %s/%s · compila G/B %llu/%llu · fallos %llu/%llu errno%d",
        name, percentage(jit, jit + interpreter).c_str(),
        percentage(interpreter, jit + interpreter).c_str(), percentage(idle, work).c_str(),
        percentage(baked, jit).c_str(), (flags & 1) ? "ON" : "OFF",
        (flags & 4) ? "listo" : "sin código",
        static_cast<unsigned long long>(stats.compilations.load()),
        static_cast<unsigned long long>(stats.bakedCompilations.load()),
        static_cast<unsigned long long>(stats.failures.load()),
        static_cast<unsigned long long>(stats.bakedFailures.load()), stats.error.load());
    std::string result = out;
    if (native) result += " · FX nativo " + percentage(native, work);
    return result;
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
                      int frames, int deviceId, int cpuMode) {
        std::lock_guard<std::mutex> guard(control_);
        stopLocked();
        cancelled_.store(false, std::memory_order_release);
        state_.store(State::booting, std::memory_order_release);
        resetStatistics();
        try {
            cpuPlacement_.prepare(cpuMode);
            frames = (frames == 512 || frames == 1024 || frames == 2048) ? frames : 1024;
            requestedFrames_.store(frames);
            stateDir_ = stateDir;
            mu_ = std::make_unique<mu2000>();
            if (!mu_->load_program(romDir + "/mu2000_flash.bin"))
                return fail("No se pudo cargar el firmware: " + mu_->error());
            auto names = std::make_shared<const xg::voice_rom>(mu_->program_rom());
            patchLayoutKnown_ = names->ok();
            patches_.configure(patchLayoutKnown_);
            std::atomic_store_explicit(&voiceNames_, std::move(names), std::memory_order_release);
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
            patches_.publish(mu_->nvram());
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
            // Buffering and callback batching are different controls. The
            // renderer already accepts variable frame counts; let AAudio
            // deliver its native bursts instead of batching a whole buffer.
            AAudioStreamBuilder_setFramesPerDataCallback(builder, AAUDIO_UNSPECIFIED);
            AAudioStreamBuilder_setBufferCapacityInFrames(builder, std::max(4096, frames * 2));
            if (deviceId > 0) AAudioStreamBuilder_setDeviceId(builder, deviceId);
            AAudioStreamBuilder_setDataCallback(builder, &Engine::dataCallback, this);
            AAudioStreamBuilder_setErrorCallback(builder, &Engine::errorCallback, this);
            result = AAudioStreamBuilder_openStream(builder, &stream_);
            AAudioStreamBuilder_delete(builder);
            if (result != AAUDIO_OK) return fail(audioError("Abrir salida", result));
            const int rate = AAudioStream_getSampleRate(stream_);
            if (AAudioStream_getFormat(stream_) != AAUDIO_FORMAT_PCM_FLOAT ||
                AAudioStream_getChannelCount(stream_) != 2 || rate < 8000 || rate > 384000)
                return fail("La salida no ofrece el formato estéreo requerido.");
            rate_.store(rate);
            observedFrames_.store(AAudioStream_getFramesPerDataCallback(stream_));
            device_.store(AAudioStream_getDeviceId(stream_));
            performanceMode_.store(AAudioStream_getPerformanceMode(stream_));
            sharingMode_.store(AAudioStream_getSharingMode(stream_));
            const int burst = std::max(1, AAudioStream_getFramesPerBurst(stream_));
            burst_.store(burst);
            const int capacity = AAudioStream_getBufferCapacityInFrames(stream_);
            const int wanted = ((std::max(frames, 2 * burst) + burst - 1) / burst) * burst;
            result = AAudioStream_setBufferSizeInFrames(stream_, std::min(capacity, wanted));
            if (result < 0) return fail(audioError("Ajustar buffer", result));
            actualBuffer_.store(AAudioStream_getBufferSizeInFrames(stream_));
            resampler_.configure_precomputed_sinc(kEngineRate, rate);
            resamplerPhases_.store(resampler_.phase_count());
            midi_.reset();
            panic_.store(false);
            midi_.setEnabled(true);
            performanceHint_.start();
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

    void detailedDiagnostics(bool enabled) {
        const bool previous = detailedDiagnostics_.exchange(enabled, std::memory_order_acq_rel);
        // The callback owns the recorder, including resets while playing.
        // Starting a capture discards old warm-up peaks so a new test is useful.
        if (enabled && !previous) diagnosticReset_.fetch_add(1, std::memory_order_release);
    }

    std::array<std::string, 2> currentSound(int channel, bool followProgramChanges) {
        const int fallback = std::clamp(channel, 0, 15); // on-screen controls use port A
        observedChannel_.store(fallback, std::memory_order_relaxed);
        followPrograms_.store(followProgramChanges, std::memory_order_relaxed);
        if (!running()) return {"Audio detenido", "Iniciá el motor para leer el sonido del firmware."};
        const auto names = std::atomic_load_explicit(&voiceNames_, std::memory_order_acquire);
        const int pcRoute = lastProgramRoute_.load(std::memory_order_relaxed);
        const int route = followProgramChanges && pcRoute >= 0 ? pcRoute : fallback;
        std::string title, details = "Puerto ";
        details += char('A' + route / 16);
        details += " · canal " + std::to_string(route % 16 + 1);
        details += followProgramChanges && pcRoute >= 0
                ? " · siguiendo Program Change" : " · canal de los controles";
        if (!names || !names->ok())
            return {"Nombre no disponible", details + "\nFirmware sin mapa de nombres reconocido."};
        int matches = 0;
        for (int part = 0; part < 64; ++part) {
            const auto data = patches_.load(part);
            if (!data.valid || data.receive_channel != route) continue;
            std::string name = rimu2000::resolve_patch_name(data, *names);
            if (name.empty()) name = "Nombre no disponible";
            const std::string label = std::string(1, char('A' + part / 16)) + std::to_string(part % 16 + 1);
            if (matches++) title += "\n";
            title += label + " · " + name;
            details += "\nParte " + label + " · programa " + std::to_string(int(data.program) + 1)
                    + " · banco MSB " + std::to_string(data.bank_msb)
                    + " / LSB " + std::to_string(data.bank_lsb);
        }
        if (!matches) return {"Sin parte receptora", details + "\nNinguna parte del firmware recibe en esa ruta."};
        return {std::move(title), std::move(details)};
    }

    std::string callbackDiagnostics() const {
        using Diagnostics = rimu2000::CallbackDiagnostics;
        Diagnostics::Snapshot snapshot;
        if (!callbackDiagnostics_.read(snapshot)) return "Actualizando el registro de bloques…";
        const auto names = std::atomic_load_explicit(&voiceNames_, std::memory_order_acquire);
        const auto percent = [](const Diagnostics::Sample& s) {
            return s.frames > 0 && s.rate > 0 ? double(s.wallNs) * s.rate / (double(s.frames) * 10000000.0) : 0.0;
        };
        std::sort(snapshot.worst.begin(), snapshot.worst.begin() + snapshot.worstCount,
                  [&](const auto& a, const auto& b) { return percent(a) > percent(b); });
        std::string result = detailedDiagnostics_.load(std::memory_order_relaxed)
                ? "Detalle de fallos de página/contextos activado."
                : "Detalle de fallos de página/contextos desactivado; se conserva el registro básico.";
        result += "\n8 mayores tiempos de trabajo / período. Un bloque >100% puede absorberse con el buffer."
                  "\nLos 4 últimos incrementos de xruns se registran aparte; la notificación puede llegar después del corte."
                  "\nVoces M/S: máximos observados por bloque, incluidas colas; no equivalen a teclas."
                  "\nJIT: deltas de intentos, incluso los que terminan bien. Intervalo: entre inicios de callbacks.\n";
        const auto append = [&](const Diagnostics::Sample& s) {
            char line[1400];
            std::snprintf(line, sizeof(line),
                "\nt=%.3f s · #%llu · %d fr/%d Hz · %.1f%%\n"
                "reloj %.3f ms / CPU %.3f ms · motor %.3f / conversión %.3f · intervalo %.3f ms\n"
                "CPU %d→%d · MIDI %llu bytes · voces M/S %d/%d · xruns +%d (total %d)\n"
                "JIT SH2 +%llu (fallos +%llu) · MEG M G/B +%llu/+%llu · S +%llu/+%llu",
                double(s.sinceStartNs) / 1000000000.0, static_cast<unsigned long long>(s.sequence),
                s.frames, s.rate, percent(s), double(s.wallNs) / 1000000.0, double(s.cpuNs) / 1000000.0,
                double(s.coreNs) / 1000000.0, double(s.convertNs) / 1000000.0, double(s.startIntervalNs) / 1000000.0,
                s.cpuStart, s.cpuEnd, static_cast<unsigned long long>(s.midiBytes), s.voicesMaster, s.voicesSlave,
                s.xrunDelta, s.xrunTotal, static_cast<unsigned long long>(s.shCompileAttempts),
                static_cast<unsigned long long>(s.shCompileFailures),
                static_cast<unsigned long long>(s.masterBuildAttempts), static_cast<unsigned long long>(s.masterBakeAttempts),
                static_cast<unsigned long long>(s.slaveBuildAttempts), static_cast<unsigned long long>(s.slaveBakeAttempts));
            result += line;
            if (s.rusageValid) {
                std::snprintf(line, sizeof(line), "\nΔ minflt/majflt %llu/%llu · cambios invol./vol. %llu/%llu",
                    static_cast<unsigned long long>(s.minorFaults), static_cast<unsigned long long>(s.majorFaults),
                    static_cast<unsigned long long>(s.involuntarySwitches), static_cast<unsigned long long>(s.voluntarySwitches));
                result += line;
            } else {
                result += "\nFallos de página/contextos: sin captura";
                if (s.rusageError) result += " (errno " + std::to_string(s.rusageError) + ")";
            }
            if (s.patchPart >= 0) {
                const auto data = rimu2000::PatchSnapshot::unpack(s.patchWord, s.patchPart);
                const auto name = names ? rimu2000::resolve_patch_name(data, *names) : std::string{};
                result += "\nParte observada " + std::string(1, char('A' + s.patchPart / 16))
                        + std::to_string(s.patchPart % 16 + 1) + " · " + (name.empty() ? "nombre no disponible" : name)
                        + " · programa " + std::to_string(int(data.program) + 1)
                        + " · MSB/LSB " + std::to_string(data.bank_msb) + "/" + std::to_string(data.bank_lsb);
            }
            result += "\n";
        };
        result += "\nBLOQUES MÁS LENTOS\n";
        for (std::size_t i = 0; i < snapshot.worstCount; ++i) append(snapshot.worst[i]);
        result += "\nÚLTIMOS INCREMENTOS DE XRUNS\n";
        if (!snapshot.xrunCount) result += "Ninguno registrado.\n";
        for (std::size_t i = 0; i < snapshot.xrunCount; ++i) append(snapshot.xruns[i]);
        result += "\nEstos datos indican coincidencias, no causas: un fallo de página no identifica la memoria afectada; "
                  "CPU inicial=final no excluye migraciones intermedias. Para una captura nueva, apagá y encendé Detalle de cortes.";
        return result;
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
        TimingWindow recent;
        const bool hasRecent = recent_.read(recent);
        if (!hasRecent) {
            recent.span = began && time >= began ? time - began : 0;
            recent.frames = samples;
            recent.elapsed = elapsed;
            recent.cpu = audioCpuNs_.load();
            recent.core = coreNs_.load();
            recent.conversion = conversionNs_.load();
            recent.callbacks = callbacks_.load();
            recent.late = late_.load();
            recent.maximum = maxPercent_.load();
            recent.cpuMask = cpuMask_.load();
        }
        const auto percentOfAudio = [rate, &recent](std::uint64_t ns) {
            return recent.frames && rate > 0
                    ? double(ns) * rate / (double(recent.frames) * 10000000.0) : 0.0;
        };
        const double blockDivisor = recent.callbacks ? double(recent.callbacks) * 1000000.0 : 1.0;
        const double budgetMs = recent.callbacks && rate > 0
                ? double(recent.frames) * 1000.0 / (double(recent.callbacks) * rate) : 0.0;
        const double audioOccupancy = recent.span
                ? double(recent.cpu) * 100.0 / double(recent.span) : 0.0;
        std::string cpus;
        for (int cpuIndex = 0; cpuIndex < 64; ++cpuIndex) {
            if ((recent.cpuMask >> cpuIndex) & 1) {
                if (!cpus.empty()) cpus += ",";
                cpus += std::to_string(cpuIndex);
            }
        }
        if (cpus.empty()) cpus = "-";
        const char* performance = performanceMode_.load() == AAUDIO_PERFORMANCE_MODE_LOW_LATENCY
                ? "LOW_LATENCY" : performanceMode_.load() == AAUDIO_PERFORMANCE_MODE_POWER_SAVING
                ? "POWER_SAVING" : "NONE";
        const char* sharing = sharingMode_.load() == AAUDIO_SHARING_MODE_EXCLUSIVE ? "EXCLUSIVE" : "SHARED";
        std::string jitText = "JIT: esperando el primer intervalo de audio.";
        if (jitPublished_.load(std::memory_order_acquire)) {
            const auto flags = shJit_.flags.load(std::memory_order_relaxed);
            char sh[256];
            std::snprintf(sh, sizeof(sh),
                "SH2 JIT %s ciclos · %s · nativo %s · listo %s · compila %llu · fallos %llu (mem %llu) errno%d",
                percentage(shJit_.jit.load(), shJit_.work.load()).c_str(),
                (flags & 1) ? "ON" : "OFF", (flags & 2) ? "sí" : "no",
                (flags & 4) ? "sí" : "no",
                static_cast<unsigned long long>(shJit_.compilations.load()),
                static_cast<unsigned long long>(shJit_.failures.load()),
                static_cast<unsigned long long>(shJit_.allocationFailures.load()), shJit_.error.load());
            jitText = std::string(sh) + "\n" + megDescription(masterJit_, "M") +
                      "\n" + megDescription(slaveJit_, "S");
        }
        char out[3072];
        std::snprintf(out, sizeof(out),
            "%s · %s %.2f s: render %.1f%% (máx %.1f%%)\n"
            "Bloque %.2f ms reloj / %.2f ms CPU / %.2f ms disponibles · motor %.1f%% + conversión %.1f%%\n"
            "Total render %.1f%% (máx %.1f%%) · CPU hilo %.1f%% / app %.1f%% (100%% = 1 núcleo)\n"
            "Xruns %d · bloques > período: reciente %llu/%llu · total %llu/%llu\n"
            "44100 → %d Hz · callback Android %d fr · buffer pedido %d / real %d fr · ráfaga %d · sinc64 fases %d\n"
            "MIDI %llu bytes · descartados %llu · pendientes %llu · salida ID %d\n"
            "Hilo %d · CPU #%d · vistas %s · %s%s prioridad %d · AAudio %s/%s\n"
            "ADPF %s · informes %llu · omitidos %llu · error %d\n"
            "%s\nARM64 · firmware original · 1 hilo de render%s",
            label, hasRecent ? "Reciente" : "Inicial", double(recent.span) / 1000000000.0,
            percentOfAudio(recent.elapsed), double(recent.maximum),
            double(recent.elapsed) / blockDivisor, double(recent.cpu) / blockDivisor, budgetMs,
            percentOfAudio(recent.core), percentOfAudio(recent.conversion),
            average, double(maxPercent_.load()), audioOccupancy, appCpu, xruns_.load(),
            static_cast<unsigned long long>(recent.late), static_cast<unsigned long long>(recent.callbacks),
            static_cast<unsigned long long>(late_.load()),
            static_cast<unsigned long long>(callbacks_.load()), rate, observedFrames_.load(), requestedFrames_.load(),
            actualBuffer_.load(), burst_.load(), resamplerPhases_.load(),
            static_cast<unsigned long long>(midiReceived_.load()),
            static_cast<unsigned long long>(dropped_.load() + internalDropped_.load()),
            static_cast<unsigned long long>(pendingMidi_.load()), device_.load(),
            audioTid_.load(), lastCpu_.load(), cpus.c_str(), schedulerName(policy_.load()),
            policy_.load() >= 0 && (policy_.load() & SCHED_RESET_ON_FORK) ? "+reset-fork" : "",
            priority_.load(), performance, sharing,
            performanceHint_.statusText(), static_cast<unsigned long long>(performanceHint_.reports()),
            static_cast<unsigned long long>(performanceHint_.dropped()), performanceHint_.error(),
            jitText.c_str(),
            nvramWarning_.load() ? "\nNo se pudieron guardar los ajustes de la sesión." : "");
        // Concatenate after formatting so even a large, sparse CPU mask is not
        // silently cut to the fixed buffer used for the other diagnostics.
        std::string result = std::string(out) + "\nVoces activas M/S " + std::to_string(voicesMaster_.load()) + "/"
                + std::to_string(voicesSlave_.load()) + " · máximos observados "
                + std::to_string(peakVoicesMaster_.load()) + "/" + std::to_string(peakVoicesSlave_.load())
                + " (incluyen colas)\nCompila: intentos en la ventana reciente; fallos: acumulados.\n"
                + cpuPlacement_.describe();
        const int xrunError = xrunError_.load(std::memory_order_relaxed);
        if (xrunError < 0) result += "\nXruns: último total válido; consulta actual no disponible (error "
                + std::to_string(xrunError) + ").";
        return result;
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
        // Session API calls and its worker never outlive the stream callback.
        performanceHint_.stop();
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
        xrunError_.store(0);
        internalDropped_.store(0); pendingMidi_.store(0); observedFrames_.store(0);
        rate_.store(0); actualBuffer_.store(0); burst_.store(0); device_.store(0);
        resamplerPhases_.store(0);
        startTime_.store(0); startCpu_.store(0); nvramWarning_.store(false);
        audioCpuNs_.store(0); coreNs_.store(0); conversionNs_.store(0); cpuMask_.store(0);
        audioTid_.store(0); lastCpu_.store(-1); policy_.store(-1); priority_.store(0);
        performanceMode_.store(AAUDIO_PERFORMANCE_MODE_NONE);
        sharingMode_.store(AAUDIO_SHARING_MODE_SHARED);
        window_ = {};
        recent_.publish(window_);
        jitBaseline_ = false;
        jitPublished_.store(false);
        shJit_.reset(); masterJit_.reset(); slaveJit_.reset();
        programTracker_.reset();
        lastProgramRoute_.store(-1);
        lastPatchTime_ = previousCallbackBegin_ = 0;
        callbackDiagnostics_.reset();
        seenDiagnosticReset_ = diagnosticReset_.load(std::memory_order_relaxed);
        voicesMaster_.store(0); voicesSlave_.store(0);
        peakVoicesMaster_.store(0); peakVoicesSlave_.store(0);
    }

    void sendPanic() {
        midi_.discard();
        programTracker_.reset();
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

    // Only the callback touches the core's ordinary (non-atomic) counters.
    // Establish the baseline before its first generated sample, excluding boot.
    void beginJitWindow() {
        previousShJit_ = mu_->cpu().jit_stats();
        previousMasterJit_ = mu_->swpm().meg_jit_stats();
        previousSlaveJit_ = mu_->swps().meg_jit_stats();
        jitBaseline_ = true;
    }

    void publishJitWindow() {
        const auto sh = mu_->cpu().jit_stats();
        shJit_.flags.store(unsigned(sh.enabled) | (unsigned(sh.native_code) << 1) |
                           (unsigned(sh.ready) << 2), std::memory_order_relaxed);
        shJit_.work.store(elapsedSince(previousShJit_.total_cycles, sh.total_cycles));
        shJit_.jit.store(elapsedSince(previousShJit_.cycles, sh.cycles));
        shJit_.interpreter.store(elapsedSince(previousShJit_.interpreter_instructions,
                                             sh.interpreter_instructions));
        shJit_.failures.store(sh.compile_failures);
        shJit_.allocationFailures.store(sh.allocation_failures);
        shJit_.error.store(sh.last_errno);
        shJit_.compilations.store(elapsedSince(previousShJit_.compile_attempts, sh.compile_attempts));
        previousShJit_ = sh;
        const auto publishMeg = [](const swp30_device::jit_statistics& current,
                                   swp30_device::jit_statistics& previous, PublishedJit& output) {
            output.flags.store(unsigned(current.enabled) | (unsigned(current.ready) << 2) |
                               (unsigned(current.baked_ready) << 3), std::memory_order_relaxed);
            output.work.store(elapsedSince(previous.samples, current.samples));
            output.jit.store(elapsedSince(previous.jit_runs, current.jit_runs));
            output.interpreter.store(elapsedSince(previous.interpreter_runs, current.interpreter_runs));
            output.idle.store(elapsedSince(previous.idle_samples, current.idle_samples));
            output.nativeFx.store(elapsedSince(previous.native_samples, current.native_samples));
            output.baked.store(elapsedSince(previous.baked_runs, current.baked_runs));
            output.failures.store(current.build_failures);
            output.bakedFailures.store(current.baked_build_failures);
            output.error.store(current.last_errno);
            output.compilations.store(elapsedSince(previous.build_attempts, current.build_attempts));
            output.bakedCompilations.store(elapsedSince(previous.baked_build_attempts, current.baked_build_attempts));
            previous = current;
        };
        publishMeg(mu_->swpm().meg_jit_stats(), previousMasterJit_, masterJit_);
        publishMeg(mu_->swps().meg_jit_stats(), previousSlaveJit_, slaveJit_);
        jitPublished_.store(true, std::memory_order_release);
    }

    static aaudio_data_callback_result_t dataCallback(
            AAudioStream* stream, void* context, void* audio, int32_t count) {
        return static_cast<Engine*>(context)->render(stream, static_cast<float*>(audio), count);
    }

    aaudio_data_callback_result_t render(AAudioStream* stream, float* output, int count) {
        if (count <= 0) return AAUDIO_CALLBACK_RESULT_CONTINUE;
        const auto begin = clockNs(CLOCK_MONOTONIC);
        const auto cpuBegin = clockNs(CLOCK_THREAD_CPUTIME_ID);
        const int firstCpu = sched_getcpu();
        if (!running_.load(std::memory_order_acquire) || cancelled_.load(std::memory_order_acquire)) {
            std::fill_n(output, std::size_t(count) * 2, 0.0f);
            cpuPlacement_.restoreCurrentThread();
            return AAUDIO_CALLBACK_RESULT_STOP;
        }
        const auto reset = diagnosticReset_.load(std::memory_order_acquire);
        if (reset != seenDiagnosticReset_) {
            callbackDiagnostics_.reset();
            seenDiagnosticReset_ = reset;
        }
        const bool detailed = detailedDiagnostics_.load(std::memory_order_acquire);
        rusage usageBefore{}, usageAfter{};
        bool usageCaptured = false;
        int usageError = 0;
        if (detailed) {
            usageCaptured = getrusage(RUSAGE_THREAD, &usageBefore) == 0;
            if (!usageCaptured) usageError = errno;
        }
        if (!jitBaseline_) beginJitWindow();
        const int tid = gettid();
        cpuPlacement_.enterAudioThread(tid);
        if (audioTid_.load(std::memory_order_relaxed) != tid ||
            (callbacks_.load(std::memory_order_relaxed) & 31) == 0) {
            audioTid_.store(tid, std::memory_order_relaxed);
            policy_.store(sched_getscheduler(0), std::memory_order_relaxed);
            sched_param parameters{};
            if (sched_getparam(0, &parameters) == 0)
                priority_.store(parameters.sched_priority, std::memory_order_relaxed);
        }
        const auto shBefore = mu_->cpu().jit_stats();
        const auto masterBefore = mu_->swpm().meg_jit_stats();
        const auto slaveBefore = mu_->swps().meg_jit_stats();
        int voicesMaster = mu_->swpm().sounding_voices(), voicesSlave = mu_->swps().sounding_voices();
        int peakMaster = voicesMaster, peakSlave = voicesSlave;
        auto boundary = clockNs(CLOCK_MONOTONIC);
        std::uint64_t coreElapsed = 0, conversionElapsed = 0;
        std::uint64_t receivedBytes = 0;
        try {
            for (int done = 0; done < count; ) {
                if (panic_.exchange(false, std::memory_order_acq_rel)) sendPanic();
                // Keep the firmware's queue bounded rather than merely moving a
                // MIDI flood from one queue to another. This also bounds how much
                // work precedes a panic message inside the firmware.
                const auto pending = mu_->midi_pending();
                const std::size_t allowance = pending < 2048 ? 2048 - pending : 0;
                const auto received = midi_.consume(allowance, [&](u8 byte, u8 port) {
                    mu_->midi_in(byte, port);
                    const int route = programTracker_.feed(byte, port);
                    if (route >= 0) lastProgramRoute_.store(route, std::memory_order_relaxed);
                });
                midiReceived_.fetch_add(received, std::memory_order_relaxed);
                receivedBytes += received;
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
                voicesMaster = mu_->swpm().sounding_voices();
                voicesSlave = mu_->swps().sounding_voices();
                peakMaster = std::max(peakMaster, voicesMaster);
                peakSlave = std::max(peakSlave, voicesSlave);
                const auto coreEnd = clockNs(CLOCK_MONOTONIC);
                coreElapsed += elapsedSince(boundary, coreEnd);
                resampler_.push(staging_.data(), need);
                resampler_.pull(output + done * 2, chunk);
                const float gain = gain_.load(std::memory_order_relaxed);
                for (int i = 0; i < chunk * 2; ++i) output[done * 2 + i] *= gain;
                boundary = clockNs(CLOCK_MONOTONIC);
                conversionElapsed += elapsedSince(coreEnd, boundary);
                done += chunk;
            }
        } catch (...) {
            std::fill_n(output, std::size_t(count) * 2, 0.0f);
            running_.store(false);
            state_.store(State::failed);
            cpuPlacement_.restoreCurrentThread();
            return AAUDIO_CALLBACK_RESULT_STOP;
        }
        voicesMaster_.store(voicesMaster, std::memory_order_relaxed);
        voicesSlave_.store(voicesSlave, std::memory_order_relaxed);
        if (peakMaster > peakVoicesMaster_.load(std::memory_order_relaxed)) peakVoicesMaster_.store(peakMaster);
        if (peakSlave > peakVoicesSlave_.load(std::memory_order_relaxed)) peakVoicesSlave_.store(peakSlave);
        if (!lastPatchTime_ || elapsedSince(lastPatchTime_, boundary) >= kPatchWindowNs) {
            patches_.publish(mu_->nvram());
            lastPatchTime_ = boundary;
        }
        rimu2000::CallbackDiagnostics::Sample sample;
        sample.sequence = callbacks_.load(std::memory_order_relaxed) + 1;
        sample.sinceStartNs = elapsedSince(startTime_.load(std::memory_order_relaxed), begin);
        sample.frames = count;
        sample.rate = rate_.load(std::memory_order_relaxed);
        sample.coreNs = coreElapsed;
        sample.convertNs = conversionElapsed;
        sample.startIntervalNs = previousCallbackBegin_ ? elapsedSince(previousCallbackBegin_, begin) : 0;
        previousCallbackBegin_ = begin;
        sample.midiBytes = receivedBytes;
        sample.voicesMaster = peakMaster;
        sample.voicesSlave = peakSlave;
        const auto shAfter = mu_->cpu().jit_stats();
        const auto masterAfter = mu_->swpm().meg_jit_stats();
        const auto slaveAfter = mu_->swps().meg_jit_stats();
        sample.shCompileAttempts = elapsedSince(shBefore.compile_attempts, shAfter.compile_attempts);
        sample.shCompileFailures = elapsedSince(shBefore.compile_failures, shAfter.compile_failures);
        sample.masterBuildAttempts = elapsedSince(masterBefore.build_attempts, masterAfter.build_attempts);
        sample.masterBakeAttempts = elapsedSince(masterBefore.baked_build_attempts, masterAfter.baked_build_attempts);
        sample.slaveBuildAttempts = elapsedSince(slaveBefore.build_attempts, slaveAfter.build_attempts);
        sample.slaveBakeAttempts = elapsedSince(slaveBefore.baked_build_attempts, slaveAfter.baked_build_attempts);
        sample.midiRoute = observedChannel_.load(std::memory_order_relaxed);
        const int lastProgram = lastProgramRoute_.load(std::memory_order_relaxed);
        if (followPrograms_.load(std::memory_order_relaxed) && lastProgram >= 0) sample.midiRoute = lastProgram;
        // Identify one observed receiving part, not every part that contributes
        // to the mix. Save its actual firmware state, not a later UI snapshot.
        for (int part = 0; patchLayoutKnown_ && part < 64; ++part) {
            const auto data = rimu2000::PatchSnapshot::capturePart(mu_->nvram(), part, true);
            if (!data.valid || data.receive_channel != sample.midiRoute) continue;
            sample.patchPart = part;
            sample.patchWord = rimu2000::PatchSnapshot::pack(data);
            break;
        }
        const int previousXruns = xruns_.load(std::memory_order_relaxed);
        const int queriedXruns = AAudioStream_getXRunCount(stream);
        // A transient API error is not a counter reset. Retain the previous
        // total so recovery cannot fabricate a new batch of xruns.
        xrunError_.store(queriedXruns < 0 ? queriedXruns : 0, std::memory_order_relaxed);
        sample.xrunTotal = queriedXruns >= 0 ? queriedXruns : previousXruns;
        sample.xrunDelta = queriedXruns >= 0 ? std::max(0, queriedXruns - previousXruns) : 0;
        if (detailed && usageCaptured) {
            sample.rusageValid = getrusage(RUSAGE_THREAD, &usageAfter) == 0;
            if (!sample.rusageValid) usageError = errno;
            if (sample.rusageValid) {
                sample.minorFaults = elapsedSince(usageBefore.ru_minflt, usageAfter.ru_minflt);
                sample.majorFaults = elapsedSince(usageBefore.ru_majflt, usageAfter.ru_majflt);
                sample.involuntarySwitches = elapsedSince(usageBefore.ru_nivcsw, usageAfter.ru_nivcsw);
                sample.voluntarySwitches = elapsedSince(usageBefore.ru_nvcsw, usageAfter.ru_nvcsw);
            }
        }
        sample.rusageError = usageError;
        const auto end = clockNs(CLOCK_MONOTONIC);
        const auto elapsed = elapsedSince(begin, end);
        const auto cpuElapsed = elapsedSince(cpuBegin, clockNs(CLOCK_THREAD_CPUTIME_ID));
        const int finalCpu = sched_getcpu();
        sample.wallNs = elapsed;
        sample.cpuNs = cpuElapsed;
        sample.cpuStart = firstCpu;
        sample.cpuEnd = finalCpu;
        callbackDiagnostics_.record(sample);
        lastCpu_.store(finalCpu, std::memory_order_relaxed);
        cpuPlacement_.observeCpu(finalCpu);
        std::uint64_t observedCpus = 0;
        if (firstCpu >= 0 && firstCpu < 64) observedCpus |= std::uint64_t(1) << firstCpu;
        if (finalCpu >= 0 && finalCpu < 64) observedCpus |= std::uint64_t(1) << finalCpu;
        cpuMask_.fetch_or(observedCpus, std::memory_order_relaxed);
        const double fraction = double(elapsed) * rate_.load(std::memory_order_relaxed) /
                                (double(count) * 1000000000.0);
        const float percent = float(fraction * 100.0);
        // Only the callback writes maxima while playing; reset is after close.
        if (percent > maxPercent_.load(std::memory_order_relaxed)) maxPercent_.store(percent);
        if (fraction > 1.0) late_.fetch_add(1, std::memory_order_relaxed);
        elapsedNs_.fetch_add(elapsed, std::memory_order_relaxed);
        audioCpuNs_.fetch_add(cpuElapsed, std::memory_order_relaxed);
        coreNs_.fetch_add(coreElapsed, std::memory_order_relaxed);
        conversionNs_.fetch_add(conversionElapsed, std::memory_order_relaxed);
        observedFrames_.store(count, std::memory_order_relaxed);
        internalDropped_.store(mu_->midi_dropped(), std::memory_order_relaxed);
        pendingMidi_.store(mu_->midi_pending(), std::memory_order_relaxed);
        frames_.fetch_add(std::uint64_t(count), std::memory_order_relaxed);
        callbacks_.fetch_add(1, std::memory_order_relaxed);
        xruns_.store(sample.xrunTotal, std::memory_order_relaxed);
        if (!window_.started) window_.started = begin;
        window_.span = elapsedSince(window_.started, end);
        window_.frames += std::uint64_t(count);
        window_.elapsed += elapsed;
        window_.cpu += cpuElapsed;
        window_.core += coreElapsed;
        window_.conversion += conversionElapsed;
        window_.callbacks++;
        window_.late += fraction > 1.0 ? 1 : 0;
        window_.maximum = std::max(window_.maximum, percent);
        window_.cpuMask |= observedCpus;
        if (window_.span >= kStatisticsWindowNs) {
            publishJitWindow();
            recent_.publish(window_);
            window_ = {};
            // Include the idle gap before the next callback in CPU occupancy.
            window_.started = end;
        }
        const auto targetNs = std::int64_t(count) * 1000000000LL /
                              std::max(1, rate_.load(std::memory_order_relaxed));
        performanceHint_.submit(tid, targetNs, std::int64_t(elapsed));
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
    rimu2000::PerformanceHint performanceHint_;
    rimu2000::AudioCpuPlacement cpuPlacement_;
    rimu2000::PatchSnapshot patches_;
    rimu2000::ProgramChangeTracker programTracker_;
    rimu2000::CallbackDiagnostics callbackDiagnostics_;
    // Immutable ROM ownership is shared only by control/UI, never acquired in RT.
    std::shared_ptr<const xg::voice_rom> voiceNames_;
    bool patchLayoutKnown_ = false; // fixed before stream start
    std::uint64_t lastPatchTime_ = 0, previousCallbackBegin_ = 0, seenDiagnosticReset_ = 0;
    std::atomic<std::uint64_t> diagnosticReset_{0};
    std::atomic<bool> detailedDiagnostics_{false}, followPrograms_{true};
    std::atomic<int> lastProgramRoute_{-1}, observedChannel_{0};
    std::atomic<int> voicesMaster_{0}, voicesSlave_{0}, peakVoicesMaster_{0}, peakVoicesSlave_{0};
    TimingWindow window_;
    PublishedWindow recent_;
    bool jitBaseline_ = false; // callback-owned; reset only after close
    sh2_device::jit_statistics previousShJit_{};
    swp30_device::jit_statistics previousMasterJit_{}, previousSlaveJit_{};
    PublishedJit shJit_, masterJit_, slaveJit_;
    std::atomic<bool> jitPublished_{false};
    std::string stateDir_;
    bool booted_ = false;
    std::atomic<bool> running_{false}, cancelled_{false}, panic_{false}, nvramWarning_{false};
    std::atomic<State> state_{State::stopped};
    std::atomic<float> gain_{0.8f}, maxPercent_{0};
    std::atomic<int> rate_{0}, requestedFrames_{1024}, observedFrames_{0}, actualBuffer_{0}, burst_{0}, device_{0}, xruns_{0};
    std::atomic<int> resamplerPhases_{0}, xrunError_{0};
    std::atomic<int> audioTid_{0}, lastCpu_{-1}, policy_{-1}, priority_{0},
        performanceMode_{AAUDIO_PERFORMANCE_MODE_NONE}, sharingMode_{AAUDIO_SHARING_MODE_SHARED};
    std::atomic<std::uint64_t> frames_{0}, elapsedNs_{0}, callbacks_{0}, late_{0}, dropped_{0},
        midiReceived_{0}, internalDropped_{0}, pendingMidi_{0}, startTime_{0}, startCpu_{0},
        audioCpuNs_{0}, coreNs_{0}, conversionNs_{0}, cpuMask_{0};
};

// Intentionally process-lifetime: close explicitly on the lifecycle worker.
// No C++ static destructor may race the Android audio service during exit.
Engine& engine() { static Engine* instance = new Engine; return *instance; }
} // namespace

extern "C" JNIEXPORT jstring JNICALL
Java_com_rinoize_rimu2000_NativeEngine_start(JNIEnv* env, jclass, jstring roms,
                                           jstring state, jint frames, jint device, jint cpuMode) {
    const auto result = engine().start(fromJava(env, roms), fromJava(env, state), frames, device, cpuMode);
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

extern "C" JNIEXPORT jobjectArray JNICALL
Java_com_rinoize_rimu2000_NativeEngine_currentSound(JNIEnv* env, jclass, jint channel, jboolean follow) {
    const auto sound = engine().currentSound(channel, follow == JNI_TRUE);
    jclass stringClass = env->FindClass("java/lang/String");
    if (!stringClass) return nullptr;
    jobjectArray result = env->NewObjectArray(2, stringClass, nullptr);
    env->DeleteLocalRef(stringClass);
    if (!result) return nullptr;
    for (int i = 0; i < 2; ++i) {
        jstring item = env->NewStringUTF(sound[i].c_str());
        if (!item) return result;
        env->SetObjectArrayElement(result, i, item);
        env->DeleteLocalRef(item);
    }
    return result;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_rinoize_rimu2000_NativeEngine_callbackDiagnostics(JNIEnv* env, jclass) {
    const auto text = engine().callbackDiagnostics();
    return env->NewStringUTF(text.c_str());
}

extern "C" JNIEXPORT void JNICALL
Java_com_rinoize_rimu2000_NativeEngine_setDetailedDiagnostics(JNIEnv*, jclass, jboolean enabled) {
    engine().detailedDiagnostics(enabled == JNI_TRUE);
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
