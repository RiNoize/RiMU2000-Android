// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include <android/api-level.h>
#include <android/performance_hint.h>
#include <dlfcn.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>

namespace rimu2000 {

// ADPF is advisory and device-specific. Native APIs start at API 33 (Java at 31).
// Runtime symbol lookup keeps the APK usable on API 26. See Google's audio guide:
// github.com/google/oboe/wiki/Using-ADPF-for-High%E2%80%90Quality-Audio-Performance
//
// The audio callback only publishes a fixed-size record. All library loading,
// session management and potentially blocking framework calls run elsewhere.
// Stop this worker AFTER AAudio has joined its callback; otherwise a late
// callback could submit work for a retired session.
class PerformanceHint {
public:
    enum class Status { stopped, unavailable, waiting, active, failed };

    ~PerformanceHint() { stop(); }

    void start() noexcept {
        stop();
        write_.store(0);
        read_.store(0);
        reports_.store(0);
        dropped_.store(0);
        error_.store(0);
        latestTid_.store(0);
        status_.store(Status::unavailable);
        if (android_get_device_api_level() < 33) return;
        library_ = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
        if (!library_) return;
        getManager_ = symbol<GetManager>("APerformanceHint_getManager");
        createSession_ = symbol<CreateSession>("APerformanceHint_createSession");
        updateTarget_ = symbol<UpdateTarget>("APerformanceHint_updateTargetWorkDuration");
        reportDuration_ = symbol<ReportDuration>("APerformanceHint_reportActualWorkDuration");
        closeSession_ = symbol<CloseSession>("APerformanceHint_closeSession");
        if (!getManager_ || !createSession_ || !updateTarget_ || !reportDuration_ || !closeSession_) {
            unload();
            return;
        }
        stopping_.store(false, std::memory_order_release);
        accepting_.store(true, std::memory_order_release);
        status_.store(Status::waiting);
        try {
            worker_ = std::thread(&PerformanceHint::run, this);
        } catch (...) {
            accepting_.store(false);
            status_.store(Status::failed);
            error_.store(-1);
            unload();
        }
    }

    void stop() noexcept {
        accepting_.store(false, std::memory_order_release);
        stopping_.store(true, std::memory_order_release);
        wake_.notify_one();
        if (worker_.joinable()) worker_.join();
        unload();
        const auto status = status_.load();
        if (status == Status::active || status == Status::waiting) status_.store(Status::stopped);
    }

    // Single producer: AAudio never invokes simultaneous data callbacks.
    // No locks, allocations, syscalls or framework calls on this path.
    void submit(std::int32_t tid, std::int64_t targetNs, std::int64_t actualNs) noexcept {
        if (!accepting_.load(std::memory_order_acquire) || tid <= 0 || targetNs <= 0 || actualNs <= 0)
            return;
        latestTid_.store(tid, std::memory_order_release);
        const auto write = write_.load(std::memory_order_relaxed);
        const auto read = read_.load(std::memory_order_acquire);
        if (write - read >= kCapacity) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        queue_[write & (kCapacity - 1)] = {tid, targetNs, actualNs};
        write_.store(write + 1, std::memory_order_release);
    }

    const char* statusText() const noexcept {
        switch (status_.load(std::memory_order_relaxed)) {
        case Status::active: return "activo";
        case Status::waiting: return "esperando hilo de audio";
        case Status::unavailable: return "no disponible";
        case Status::failed: return "no disponible (falló la sesión)";
        default: return "detenido";
        }
    }
    int error() const noexcept { return error_.load(std::memory_order_relaxed); }
    std::uint64_t reports() const noexcept { return reports_.load(std::memory_order_relaxed); }
    std::uint64_t dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }

private:
    using Manager = APerformanceHintManager;
    using Session = APerformanceHintSession;
    using GetManager = Manager* (*)();
    using CreateSession = Session* (*)(Manager*, const std::int32_t*, std::size_t, std::int64_t);
    using UpdateTarget = int (*)(Session*, std::int64_t);
    using ReportDuration = int (*)(Session*, std::int64_t);
    using CloseSession = void (*)(Session*);
    struct Work { std::int32_t tid; std::int64_t targetNs, actualNs; };
    static constexpr std::uint32_t kCapacity = 64;

    template<class Function> Function symbol(const char* name) noexcept {
        return reinterpret_cast<Function>(dlsym(library_, name));
    }
    void unload() noexcept {
        if (library_) dlclose(library_);
        library_ = nullptr;
    }

    void run() noexcept {
        Manager* manager = getManager_();
        Session* session = nullptr;
        std::int32_t sessionTid = 0;
        std::int64_t target = 0;
        if (!manager) {
            status_.store(Status::unavailable);
            accepting_.store(false, std::memory_order_release);
            return;
        }
        while (!stopping_.load(std::memory_order_acquire)) {
            const auto read = read_.load(std::memory_order_relaxed);
            if (read == write_.load(std::memory_order_acquire)) {
                // The callback does not notify a condition variable or take its
                // mutex. This low-rate control wake is independent of rendering.
                std::unique_lock<std::mutex> lock(waitMutex_);
                wake_.wait_for(lock, std::chrono::milliseconds(10), [this] {
                    return stopping_.load(std::memory_order_acquire);
                });
                continue;
            }
            const Work work = queue_[read & (kCapacity - 1)];
            read_.store(read + 1, std::memory_order_release);
            // AAudio may replace its callback thread. Discard delayed reports
            // belonging to the retired thread before asking ADPF to register it.
            if (work.tid != latestTid_.load(std::memory_order_acquire)) continue;
            if (!session || work.tid != sessionTid) {
                if (session) closeSession_(session);
                session = createSession_(manager, &work.tid, 1, work.targetNs);
                if (!session) {
                    if (work.tid != latestTid_.load(std::memory_order_acquire)) continue;
                    status_.store(Status::unavailable);
                    accepting_.store(false, std::memory_order_release);
                    break;
                }
                sessionTid = work.tid;
                target = work.targetNs;
                status_.store(Status::active);
            }
            int result = 0;
            if (target != work.targetNs) {
                result = updateTarget_(session, work.targetNs);
                if (!result) target = work.targetNs;
            }
            if (!result) result = reportDuration_(session, work.actualNs);
            if (result) {
                if (work.tid != latestTid_.load(std::memory_order_acquire)) {
                    closeSession_(session);
                    session = nullptr;
                    continue;
                }
                error_.store(result);
                status_.store(Status::failed);
                accepting_.store(false, std::memory_order_release);
                break;
            }
            reports_.fetch_add(1, std::memory_order_relaxed);
        }
        if (session) closeSession_(session);
        if (stopping_.load() && status_.load() == Status::active)
            status_.store(Status::stopped);
    }

    void* library_ = nullptr;
    GetManager getManager_ = nullptr;
    CreateSession createSession_ = nullptr;
    UpdateTarget updateTarget_ = nullptr;
    ReportDuration reportDuration_ = nullptr;
    CloseSession closeSession_ = nullptr;
    std::thread worker_;
    std::mutex waitMutex_;
    std::condition_variable wake_;
    std::array<Work, kCapacity> queue_{};
    alignas(64) std::atomic<std::uint32_t> write_{0};
    alignas(64) std::atomic<std::uint32_t> read_{0};
    std::atomic<bool> stopping_{true}, accepting_{false};
    std::atomic<Status> status_{Status::stopped};
    std::atomic<int> error_{0};
    std::atomic<std::int32_t> latestTid_{0};
    std::atomic<std::uint64_t> reports_{0}, dropped_{0};
};
} // namespace rimu2000
