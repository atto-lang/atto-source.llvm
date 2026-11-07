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
// need bounded per-tenant/actor cleanup admission. Stalled value destructors
// can still delay the single CONTROL worker; pinned reads are polled without
// blocking other actors' cleanup.

#include "oreslang/runtime/NativeActorScheduler.h"
#include "oreslang/runtime/RequestLocalStopGroupV1.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
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
        std::atomic<bool> claimed{false};
        std::atomic<bool> finished{false};
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

    // Preferred trusted-ingress factory. A request scope never escapes to
    // its caller unless the exact actor-generation registry accepted it.
    // On STOP/rejection it is immediately closed before being discarded.
    // Identities still MUST originate from trusted ingress; this is not a
    // source-language constructor or tenant/isolate provenance proof.
    RequestContextLocalV1::Ptr open_registered(
            RequestIdentityV1 identity,
            TaskCancellation::Ptr cancellation) {
        if (!identity.valid() || !cancellation) return {};
        auto scope = RequestContextLocalV1::open(
            identity, std::move(cancellation));
        if (!scope) return {};
        if (!register_request(identity.actor, scope)) {
            scope->close();
            return {};
        }
        return scope;
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
                    return target->finished.load(std::memory_order_acquire);
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
            std::size_t count = 0;
            bool shutdown = false;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                // Bounded poll prevents lost wakes. STOP's atomic-only
                // callback never needs this registry mutex or a scheduler
                // callback. Entries are stable while worker is alive.
                changed_.wait_for(lock, std::chrono::milliseconds(10));
                count = entries_.size();
                shutdown = shutdown_;
            }
            bool pending = false;
            for (std::size_t i = 0; i < count; ++i) {
                Entry* selected = nullptr;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    selected = entries_[i].get();
                }
                if (!selected->notified.load(std::memory_order_acquire) ||
                    selected->finished.load(std::memory_order_acquire))
                    continue;
                pending = true;
                if (!selected->claimed.exchange(
                        true, std::memory_order_acq_rel)) {
                    bool started = false;
                    try {
                        started = selected->group.begin_external_cleanup_without_wait();
                    } catch (...) {
                        started = false;
                    }
                    if (!started) {
                        selected->cleaned.store(false, std::memory_order_release);
                        selected->finished.store(true, std::memory_order_release);
                        changed_.notify_all();
                        continue;
                    }
                }
                // No wait for a pinned copy or actor DONE: actor B can be
                // reclaimed even while an earlier actor A read is paused.
                bool done = false;
                try {
                    done = selected->group.poll_external_cleanup_finished();
                } catch (...) {
                    done = false;
                }
                if (done) {
                    selected->cleaned.store(true, std::memory_order_release);
                    selected->finished.store(true, std::memory_order_release);
                    changed_.notify_all();
                }
            }
            if (shutdown && !pending) break;
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
