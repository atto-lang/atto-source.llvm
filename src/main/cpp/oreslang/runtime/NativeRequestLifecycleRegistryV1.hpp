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
// ingress. Destroying the registry initiates sealed STOP for any still-live
// attached actor, then waits for scope quiescence and actor DONE.
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
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace oreslang::runtime {

class NativeRequestLifecycleRegistryV1 final {
    // Scheduler STOP callbacks execute under its lifecycle mutex, so the
    // notice flag MUST compile to a lock-free atomic store on this target.
    static_assert(std::atomic<bool>::is_always_lock_free,
                  "native STOP notices require a lock-free atomic flag");
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
        // CONTROL-only retirement: a registry cannot silently disappear while
        // attached actors can still admit RequestLocal operations. The host
        // must keep the scheduler alive until this destructor returns.
        // Killing this registry from its CONTROL thread would self-join.
        if (ores_native_actor_scheduler_is_any_current_carrier_v1() ||
            std::this_thread::get_id() == control_.get_id())
            std::terminate();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            shutdown_ = true; // Reject racing ingress before unregistration.
            for (const auto& entry : entries_) {
                // Lock order is registry -> scheduler. Scheduler STOP notice
                // callbacks only do a lock-free atomic store: never acquire
                // this registry mutex or destroy a C++ value.
                if (!ores_native_actor_scheduler_unregister_stop_notice_v1(
                        scheduler_, entry->actor, &stop_notice,
                        entry.get())) std::terminate();
                // After unregister, a STOP racing the shutdown is fully
                // serialized. If it did not win, publish sealed STOP here.
                // Both cases require CONTROL cleanup; no leaked live scope
                // is permitted after this registry's lifetime ends.
                if (!entry->notified.load(std::memory_order_acquire) &&
                    !ores_native_actor_scheduler_is_stopping_v1(
                        scheduler_, entry->actor) &&
                    !ores_native_actor_scheduler_stop_sealed(
                        scheduler_, entry->actor))
                    std::terminate();
                entry->notified.store(true, std::memory_order_release);
            }
        }
        changed_.notify_all();
        if (control_.joinable()) control_.join();
        // Never return from teardown if a failed cleanup could leave
        // caller-held request scopes live without a lifecycle observer.
        for (const auto& entry : entries_)
            if (!entry->finished.load(std::memory_order_acquire) ||
                !entry->cleaned.load(std::memory_order_acquire))
                std::terminate();
    }

    // Trusted host CONTROL bootstrap: register a pending-envelope
    // revocation callback before any attached actor can STOP. The callback
    // executes exactly once per stopped generation on the CONTROL worker,
    // AFTER request admission has closed and OUTSIDE the registry/scheduler
    // locks. It must be nonblocking and reclaim values outside its table
    // mutex; captured state must be lifetime-safe (weak_ptr recommended).
    // No callback is accepted after a STOP notification or shutdown.
    using StopCleanupV1 = std::function<void(OresNativeActorRef)>;
    using StopCleanupTokenV1 = std::shared_ptr<StopCleanupV1>;
    bool register_stop_cleanup(
            StopCleanupV1 cleanup, StopCleanupTokenV1* token = nullptr) {
        if (!scheduler_ || !cleanup ||
            ores_native_actor_scheduler_is_any_current_carrier_v1())
            return false;
        auto hook = std::make_shared<StopCleanupV1>(std::move(cleanup));
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_ || stop_cleanups_.size() >= 64) return false;
        // A STOP of an unrelated actor MUST NOT poison future handoff
        // tables serving remaining live generations. This hook is installed
        // before the NEW table can publish any envelope; a prior STOP has
        // no ticket in that new table to reclaim. The dual-actor scheduler
        // commit fence rejects any subsequent publication to the stopped
        // incarnation. Hooks may observe an already-queued STOP, which is
        // harmless for an initially empty table.
        stop_cleanups_.push_back(hook);
        if (token) *token = std::move(hook);
        return true;
    }

    // An owning handoff table must deregister its cleanup hook before its
    // destructor releases State. The worker snapshots shared tokens under
    // the registry lock and invokes them outside it. Concurrent unregister
    // therefore cannot invalidate an in-flight token; the weak State still
    // prevents callback UAF if the table was destroyed.
    bool unregister_stop_cleanup(const StopCleanupTokenV1& token) {
        if (!token || ores_native_actor_scheduler_is_any_current_carrier_v1())
            return false;
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = stop_cleanups_.begin();
             it != stop_cleanups_.end(); ++it) {
            if (*it != token) continue;
            stop_cleanups_.erase(it);
            return true;
        }
        return false;
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
        // A value destructor can run on the CONTROL worker. Waiting for
        // its own cleanup receipt here would deadlock that worker.
        if (ores_native_actor_scheduler_is_any_current_carrier_v1() ||
            std::this_thread::get_id() == control_.get_id())
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
                    if (started) {
                        // Snapshot callback pointers while holding the
                        // registry lock, invoke only after releasing it.
                        // At most 64 hooks; no scheduler carrier is held.
                        try {
                            std::vector<std::shared_ptr<StopCleanupV1>> hooks;
                            {
                                std::lock_guard<std::mutex> lock(mutex_);
                                hooks = stop_cleanups_;
                            }
                            for (const auto& hook : hooks)
                                (*hook)(selected->actor);
                        } catch (...) {
                            started = false;
                        }
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
    std::vector<std::shared_ptr<StopCleanupV1>> stop_cleanups_;
    bool shutdown_ = false;
    std::thread control_;
};

} // namespace oreslang::runtime
