#pragma once

// HOST-ONLY runtime-owned per-scheduler actor lifecycle registry (v1).
//
// One CONTROL worker per NativeActorScheduler, NOT one worker per actor.
// A scheduler STOP/retire notice ONLY stores an atomic flag while holding
// the scheduler mutex. The CONTROL worker observes the flag, closes all
// registered RequestLocal scopes, waits for in-flight values to quiesce and
// joins actor DONE. It never runs cleanup or guest destructors on carriers
// or while holding the scheduler/cancellation/registry locks.
//
// Boundaries: caller MUST attach each exact actor generation before request
// ingress and keep this registry alive until its registered actors terminate.
// NativeActorScheduler must outlive the registry and its callbacks. This is
// a host integration building block, NOT the guest ABI, compiler ownership,
// tenant issuer, or hot-upgrade proof. Single-worker cleanup is serial; one
// blocked actor can delay unrelated actor reclamation. Production schedulers
// need bounded per-tenant/actor cleanup admission and independent progress.

#include "oreslang/runtime/NativeActorScheduler.h"
#include "oreslang/runtime/RequestLocalStopGroupV1.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace oreslang::runtime {

class NativeRequestLifecycleRegistryV1 final {
    struct Entry final {
        explicit Entry(OresNativeActorScheduler* scheduler,
                       OresNativeActorRef actor)
            : actor(actor), group(scheduler, actor) {}
        const OresNativeActorRef actor;
        RequestLocalStopGroupV1 group;
        std::atomic<bool> notified{false};
        std::atomic<bool> processed{false};
        std::atomic<bool> cleaned{false};
    };

public:
    explicit NativeRequestLifecycleRegistryV1(
            OresNativeActorScheduler* scheduler)
        : scheduler_(scheduler),
          control_([this] { control_loop(); }) {}
    NativeRequestLifecycleRegistryV1(
        const NativeRequestLifecycleRegistryV1&) = delete;
    NativeRequestLifecycleRegistryV1& operator=(
        const NativeRequestLifecycleRegistryV1&) = delete;

    ~NativeRequestLifecycleRegistryV1() {
        // No caller may destroy the scheduler or registry while a callback
        // or a CONTROL cleanup is active. The host owns lifetime ordering.
        if (ores_native_actor_scheduler_is_any_current_carrier_v1())
            std::terminate();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            shutdown_ = true;
        }
        changed_.notify_all();
        if (control_.joinable()) control_.join();
        // Registration is synchronized with STOP notifications by scheduler
        // mutex. No callback can run after a successful unregister.
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& entry : entries_)
            if (!ores_native_actor_scheduler_unregister_stop_notice_v1(
                    scheduler_, entry->actor, &stop_notice,
                    entry.get())) std::terminate();
    }

    bool attach(OresNativeActorRef actor) {
        if (!scheduler_ || !actor.scheduler_domain || !actor.generation ||
            ores_native_actor_scheduler_is_any_current_carrier_v1())
            return false;
        auto entry = std::make_unique<Entry>(scheduler_, actor);
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_) return false;
        for (const auto& old : entries_)
            if (same_actor(old->actor, actor)) return false;
        // Allocate registry capacity first to ensure a failing allocation
        // never leaves a dangling callback at an unrecorded Entry address.
        entries_.reserve(entries_.size() + 1);
        if (!ores_native_actor_scheduler_register_stop_notice_v1(
                scheduler_, actor, &stop_notice, entry.get()))
            return false;
        entries_.push_back(std::move(entry)); // capacity already reserved
        return true;
    }

    bool register_request(OresNativeActorRef actor,
                          const RequestContextLocalV1::Ptr& request) {
        if (!request || !request->belongs_to(actor)) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_) return false;
        for (const auto& entry : entries_) {
            if (!same_actor(entry->actor, actor)) continue;
            // If STOP races registration, either it wins here (denial) or
            // the worker observes the earlier registration and closes it.
            if (entry->notified.load(std::memory_order_acquire) ||
                ores_native_actor_scheduler_is_stopping_v1(
                    scheduler_, actor))
                return false;
            if (!entry->group.register_request(request)) return false;
            return !entry->notified.load(std::memory_order_acquire);
        }
        return false;
    }

    // CONTROL-only test/administration receipt. A successful return proves
    // request close + in-flight quiescence + exact actor DONE, not guest parity.
    bool wait_cleaned(OresNativeActorRef actor,
                      std::chrono::milliseconds timeout) {
        if (ores_native_actor_scheduler_is_any_current_carrier_v1())
            return false;
        std::unique_lock<std::mutex> lock(mutex_);
        for (const auto& entry : entries_)
            if (same_actor(entry->actor, actor)) {
                const Entry* target = entry.get();
                return changed_.wait_for(lock, timeout, [&] {
                    return target->processed.load(std::memory_order_acquire);
                }) && target->cleaned.load(std::memory_order_acquire);
            }
        return false;
    }

    std::size_t attached_actor_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return entries_.size();
    }

private:
    static bool same_actor(OresNativeActorRef a, OresNativeActorRef b) {
        return a.scheduler_domain == b.scheduler_domain &&
               a.actor_slot == b.actor_slot && a.generation == b.generation;
    }
    static void stop_notice(void* opaque) noexcept {
        // Native scheduler lock HELD. Strictly one atomic operation: no
        // callback, cv, lock, allocation, guest code or destructor.
        static_cast<Entry*>(opaque)->notified.store(
            true, std::memory_order_release);
    }

    void control_loop() noexcept {
        for (;;) {
            Entry* selected = nullptr;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                // Bounded poll prevents lost-wake races without acquiring
                // the registry mutex in the scheduler's STOP callback.
                changed_.wait_for(lock, std::chrono::milliseconds(10));
                for (const auto& entry : entries_)
                    if (entry->notified.load(std::memory_order_acquire) &&
                        !entry->processed.load(std::memory_order_relaxed)) {
                        entry->processed.store(true, std::memory_order_relaxed);
                        selected = entry.get();
                        break;
                    }
                if (!selected && shutdown_) break;
            }
            if (!selected) continue;
            bool success = false;
            try {
                success = selected->group.finish_after_external_stop();
            } catch (...) {
                success = false; // never terminate the CONTROL worker
            }
            selected->cleaned.store(success, std::memory_order_release);
            changed_.notify_all();
        }
    }

    OresNativeActorScheduler* const scheduler_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<std::unique_ptr<Entry>> entries_;
    bool shutdown_ = false;
    std::thread control_;
};

} // namespace oreslang::runtime
