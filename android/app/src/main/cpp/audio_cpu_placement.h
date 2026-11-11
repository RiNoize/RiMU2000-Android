// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <sched.h>
#include <string>
#include <unistd.h>

namespace rimu2000 {
namespace cpu_placement {

struct CpuMetadata {
    bool present = false;
    int capacity = 0;
    int maximumKhz = 0;
};
using Topology = std::array<CpuMetadata, CPU_SETSIZE>;

enum class Criterion { none, capacity, maximumFrequency };
enum class Reason { none, emptyMask, uniformCapacity, uniformFrequency, incompleteMetadata };

struct Selection {
    cpu_set_t requested{};
    Criterion criterion = Criterion::none;
    Reason reason = Reason::none;
    bool change = false;
};

// Pure selection: the caller's current affinity is the entire candidate set.
// Never substitute missing values with zero or mix capacity and frequency.
inline Selection selectPerformance(const Topology& topology, const cpu_set_t& allowed) noexcept {
    Selection result;
    bool any = false, completeCapacity = true, completeFrequency = true;
    int minimumCapacity = INT_MAX, maximumCapacity = 0;
    int minimumFrequency = INT_MAX, maximumFrequency = 0;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (!CPU_ISSET(cpu, &allowed)) continue;
        any = true;
        const auto& item = topology[cpu];
        completeCapacity &= item.present && item.capacity > 0;
        completeFrequency &= item.present && item.maximumKhz > 0;
        minimumCapacity = std::min(minimumCapacity, item.capacity);
        maximumCapacity = std::max(maximumCapacity, item.capacity);
        minimumFrequency = std::min(minimumFrequency, item.maximumKhz);
        maximumFrequency = std::max(maximumFrequency, item.maximumKhz);
    }
    if (!any) {
        result.reason = Reason::emptyMask;
        return result;
    }
    if (completeCapacity) {
        result.criterion = Criterion::capacity;
        if (minimumCapacity == maximumCapacity) {
            result.reason = Reason::uniformCapacity;
            return result;
        }
    } else if (completeFrequency) {
        result.criterion = Criterion::maximumFrequency;
        if (minimumFrequency == maximumFrequency) {
            result.reason = Reason::uniformFrequency;
            return result;
        }
    } else {
        result.reason = Reason::incompleteMetadata;
        return result;
    }
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (!CPU_ISSET(cpu, &allowed)) continue;
        const bool selected = result.criterion == Criterion::capacity
                ? topology[cpu].capacity == maximumCapacity
                : topology[cpu].maximumKhz == maximumFrequency;
        if (selected) CPU_SET(cpu, &result.requested);
    }
    result.change = true;
    return result;
}

// All file access is restricted to prepare(), before AAudio starts its thread.
inline int readPositiveInteger(const char* path) noexcept {
    FILE* file = std::fopen(path, "r");
    if (!file) return 0;
    char text[64]{};
    const std::size_t length = std::fread(text, 1, sizeof(text) - 1, file);
    const bool tooLong = std::fgetc(file) != EOF;
    const bool failed = std::ferror(file);
    std::fclose(file);
    if (failed || tooLong || !length || text[0] < '0' || text[0] > '9') return 0;
    char* end = nullptr;
    errno = 0;
    const long value = std::strtol(text, &end, 10);
    if (errno || value <= 0 || value > INT_MAX) return 0;
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') ++end;
    return *end == '\0' ? int(value) : 0;
}

inline Topology discoverTopology() noexcept {
    Topology result{};
    constexpr const char* root = "/sys/devices/system/cpu";
    DIR* directory = opendir(root);
    if (!directory) return result;
    while (const dirent* entry = readdir(directory)) {
        const char* name = entry->d_name;
        if (std::strncmp(name, "cpu", 3) != 0 || name[3] < '0' || name[3] > '9') continue;
        char* end = nullptr;
        errno = 0;
        const long cpu = std::strtol(name + 3, &end, 10);
        if (errno || *end || cpu < 0 || cpu >= CPU_SETSIZE) continue;
        auto& item = result[std::size_t(cpu)];
        item.present = true;
        char path[160];
        std::snprintf(path, sizeof(path), "%s/cpu%ld/cpu_capacity", root, cpu);
        item.capacity = readPositiveInteger(path);
        std::snprintf(path, sizeof(path), "%s/cpu%ld/cpufreq/cpuinfo_max_freq", root, cpu);
        item.maximumKhz = readPositiveInteger(path);
    }
    closedir(directory);
    return result;
}

inline std::string maskText(const cpu_set_t& mask) {
    std::string result;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (!CPU_ISSET(cpu, &mask)) continue;
        const int first = cpu;
        while (cpu + 1 < CPU_SETSIZE && CPU_ISSET(cpu + 1, &mask)) ++cpu;
        if (!result.empty()) result += ',';
        result += std::to_string(first);
        if (cpu != first) result += '-' + std::to_string(cpu);
    }
    return result.empty() ? "—" : result;
}

} // namespace cpu_placement

class AudioCpuPlacement {
public:
    // Immutable configuration for this stream. Called after the old stream has
    // joined, so none of the ordinary callback-owned state can race this reset.
    void prepare(int mode) noexcept {
        topology_ = cpu_placement::discoverTopology();
        mode_ = mode == 1 ? 1 : 0;
        ownerTid_ = 0;
        changed_ = false;
        original_ = {};
        snapshot_ = {};
        snapshot_.mode = mode_;
        publish();
        observeCpu(-1);
    }

    // Self-only syscalls avoid a stale/recycled TID target. They run once when
    // a new AAudio callback thread appears; normal callbacks just compare TID.
    // There is no filesystem access, allocation, mutex, or priority change here.
    void enterAudioThread(int tid) noexcept {
        if (ownerTid_ == tid) return;
        ownerTid_ = tid;
        changed_ = false;
        original_ = {};
        snapshot_ = {};
        snapshot_.mode = mode_;
        snapshot_.tid = tid;
        if (sched_getaffinity(0, sizeof(original_), &original_) != 0) {
            snapshot_.error = errno;
            snapshot_.status = Status::readFailed;
            publish();
            return;
        }
        snapshot_.allowed = original_;
        snapshot_.effective = original_;
        if (mode_ == 0) {
            snapshot_.status = Status::automatic;
            publish();
            return;
        }
        const auto selected = cpu_placement::selectPerformance(topology_, original_);
        snapshot_.criterion = selected.criterion;
        snapshot_.reason = selected.reason;
        snapshot_.requested = selected.requested;
        if (!selected.change) {
            snapshot_.status = Status::noSelection;
            publish();
            return;
        }
        const int result = sched_setaffinity(0, sizeof(selected.requested), &selected.requested);
        snapshot_.error = result == 0 ? 0 : errno;
        changed_ = result == 0;
        snapshot_.status = changed_ ? Status::applied : Status::applyFailed;
        // A successful set can be narrowed by cpusets/online CPUs. Report the
        // actual readback, not the requested mask, even after a failed set.
        snapshot_.effective = {};
        if (sched_getaffinity(0, sizeof(snapshot_.effective), &snapshot_.effective) != 0) {
            if (!snapshot_.error) snapshot_.error = errno;
            snapshot_.status = Status::verifyFailed;
            // If verification fails after a change, request the original mask
            // in place where possible instead of claiming unverified success.
            if (changed_) restoreSelf();
        }
        publish();
    }

    // Best effort when our callback returns STOP. If AAudio stops without a
    // final callback, close joins/destroys that thread and its affinity with it.
    // Never target a numeric TID from a different control thread.
    void restoreCurrentThread() noexcept {
        if (!changed_ || ownerTid_ != gettid()) return;
        restoreSelf();
        snapshot_.status = changed_ ? Status::restoreFailed
                : snapshot_.restoreError ? Status::restoreUnverified : Status::restored;
        publish();
    }

    void observeCpu(int cpu) noexcept {
        const bool known = cpu >= 0 && cpu < CPU_SETSIZE && topology_[cpu].present;
        observedSerial_.fetch_add(1, std::memory_order_acq_rel);
        observedCpu_.store(cpu, std::memory_order_relaxed);
        observedCapacity_.store(known ? topology_[cpu].capacity : 0, std::memory_order_relaxed);
        observedMaximumKhz_.store(known ? topology_[cpu].maximumKhz : 0, std::memory_order_relaxed);
        observedSerial_.fetch_add(1, std::memory_order_release);
    }

    // UI only: uses atomic copies and never touches live callback state.
    std::string describe() const {
        Snapshot data;
        if (!read(data)) return "CPU: actualizando diagnóstico.";
        std::string result = data.mode == 1 ? "CPU Rendimiento" : "CPU Automático";
        result += " · ";
        switch (data.status) {
        case Status::waiting: result += "esperando hilo de audio"; break;
        case Status::automatic: result += "sin restricción adicional"; break;
        case Status::applied: result += "aplicado"; break;
        case Status::noSelection:
            result += "Automático: ";
            switch (data.reason) {
            case cpu_placement::Reason::emptyMask: result += "máscara vacía"; break;
            case cpu_placement::Reason::uniformCapacity: result += "capacidad uniforme entre CPUs permitidas"; break;
            case cpu_placement::Reason::uniformFrequency: result += "frecuencia máxima uniforme"; break;
            default: result += "metadatos incompletos"; break;
            }
            break;
        case Status::readFailed: result += "Automático: no se pudo leer afinidad"; break;
        case Status::applyFailed: result += "Automático: el sistema rechazó la afinidad"; break;
        case Status::verifyFailed: result += "no se pudo confirmar la afinidad"; break;
        case Status::restored: result += "restauración aceptada"; break;
        case Status::restoreFailed: result += "no se pudo restaurar la máscara"; break;
        case Status::restoreUnverified: result += "restauración solicitada; no se pudo confirmar"; break;
        }
        if (data.criterion == cpu_placement::Criterion::capacity) result += " · criterio capacidad";
        if (data.criterion == cpu_placement::Criterion::maximumFrequency)
            result += " · criterio frecuencia máxima (aproximación)";
        result += " · errno" + std::to_string(data.error);
        if (data.restoreError) result += " · restauración errno" + std::to_string(data.restoreError);
        result += "\nÚltima afinidad leída: permitidas al inicio " + cpu_placement::maskText(data.allowed)
                + " · solicitadas " + cpu_placement::maskText(data.requested)
                + " · efectivas " + cpu_placement::maskText(data.effective);
        int cpu = -1, capacity = 0, maximumKhz = 0;
        for (int attempt = 0; attempt < 2; ++attempt) {
            const auto before = observedSerial_.load(std::memory_order_acquire);
            if (before & 1) continue;
            const int readCpu = observedCpu_.load(std::memory_order_relaxed);
            const int readCapacity = observedCapacity_.load(std::memory_order_relaxed);
            const int readKhz = observedMaximumKhz_.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (before == observedSerial_.load(std::memory_order_relaxed)) {
                cpu = readCpu; capacity = readCapacity; maximumKhz = readKhz;
                break;
            }
        }
        result += "\nCPU #" + std::to_string(cpu) + " · capacidad "
                + (capacity > 0 ? std::to_string(capacity) : "no disponible") + " · máx ";
        result += maximumKhz > 0 ? std::to_string(maximumKhz / 1000) + " MHz" : "no disponible";
        return result;
    }

private:
    enum class Status { waiting, automatic, noSelection, applied, readFailed,
                        applyFailed, verifyFailed, restored, restoreFailed, restoreUnverified };
    struct Snapshot {
        int mode = 0, tid = 0, error = 0, restoreError = 0;
        cpu_placement::Criterion criterion = cpu_placement::Criterion::none;
        cpu_placement::Reason reason = cpu_placement::Reason::none;
        Status status = Status::waiting;
        cpu_set_t allowed{}, requested{}, effective{};
    };
    static constexpr std::size_t kMaskWords = (CPU_SETSIZE + 63) / 64;
    using AtomicMask = std::array<std::atomic<std::uint64_t>, kMaskWords>;

    static void storeMask(AtomicMask& target, const cpu_set_t& source) noexcept {
        for (std::size_t word = 0; word < kMaskWords; ++word) {
            std::uint64_t bits = 0;
            for (int bit = 0; bit < 64; ++bit) {
                const auto cpu = int(word * 64 + std::size_t(bit));
                if (cpu < CPU_SETSIZE && CPU_ISSET(cpu, &source)) bits |= std::uint64_t(1) << bit;
            }
            target[word].store(bits, std::memory_order_relaxed);
        }
    }
    static cpu_set_t loadMask(const AtomicMask& source) noexcept {
        cpu_set_t result{};
        for (std::size_t word = 0; word < kMaskWords; ++word) {
            const auto bits = source[word].load(std::memory_order_relaxed);
            for (int bit = 0; bit < 64; ++bit) {
                const auto cpu = int(word * 64 + std::size_t(bit));
                if (cpu < CPU_SETSIZE && ((bits >> bit) & 1)) CPU_SET(cpu, &result);
            }
        }
        return result;
    }
    void publish() noexcept {
        serial_.fetch_add(1, std::memory_order_acq_rel);
        publishedMode_.store(snapshot_.mode, std::memory_order_relaxed);
        publishedTid_.store(snapshot_.tid, std::memory_order_relaxed);
        error_.store(snapshot_.error, std::memory_order_relaxed);
        restoreError_.store(snapshot_.restoreError, std::memory_order_relaxed);
        criterion_.store(snapshot_.criterion, std::memory_order_relaxed);
        reason_.store(snapshot_.reason, std::memory_order_relaxed);
        status_.store(snapshot_.status, std::memory_order_relaxed);
        storeMask(allowed_, snapshot_.allowed);
        storeMask(requested_, snapshot_.requested);
        storeMask(effective_, snapshot_.effective);
        serial_.fetch_add(1, std::memory_order_release);
    }
    bool read(Snapshot& result) const noexcept {
        for (int attempt = 0; attempt < 2; ++attempt) {
            const auto before = serial_.load(std::memory_order_acquire);
            if (before & 1) continue;
            result.mode = publishedMode_.load(std::memory_order_relaxed);
            result.tid = publishedTid_.load(std::memory_order_relaxed);
            result.error = error_.load(std::memory_order_relaxed);
            result.restoreError = restoreError_.load(std::memory_order_relaxed);
            result.criterion = criterion_.load(std::memory_order_relaxed);
            result.reason = reason_.load(std::memory_order_relaxed);
            result.status = status_.load(std::memory_order_relaxed);
            result.allowed = loadMask(allowed_);
            result.requested = loadMask(requested_);
            result.effective = loadMask(effective_);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (before == serial_.load(std::memory_order_relaxed)) return true;
        }
        return false;
    }
    void restoreSelf() noexcept {
        if (sched_setaffinity(0, sizeof(original_), &original_) == 0) {
            changed_ = false;
            snapshot_.restoreError = 0;
        } else {
            snapshot_.restoreError = errno;
        }
        snapshot_.effective = {};
        if (sched_getaffinity(0, sizeof(snapshot_.effective), &snapshot_.effective) != 0
                && !snapshot_.restoreError) snapshot_.restoreError = errno;
    }

    cpu_placement::Topology topology_{};
    int mode_ = 0, ownerTid_ = 0;
    bool changed_ = false;
    cpu_set_t original_{};
    Snapshot snapshot_{};
    std::atomic<std::uint64_t> serial_{0}, observedSerial_{0};
    std::atomic<int> publishedMode_{0}, publishedTid_{0}, error_{0}, restoreError_{0};
    std::atomic<cpu_placement::Criterion> criterion_{cpu_placement::Criterion::none};
    std::atomic<cpu_placement::Reason> reason_{cpu_placement::Reason::none};
    std::atomic<Status> status_{Status::waiting};
    AtomicMask allowed_{}, requested_{}, effective_{};
    std::atomic<int> observedCpu_{-1}, observedCapacity_{0}, observedMaximumKhz_{0};
};
} // namespace rimu2000
