#pragma once

// Trusted host-only actor-incarnation RequestLocal registry.
//
// The scheduler calls on_termination under its lifecycle mutex. That callback
// ONLY sets preallocated atomics and cannot allocate, lock, run guest code,
// execute request destruction, wait or reenter the scheduler. Registration
// commits under the SAME scheduler mutex via commit_if_live_v1, so a STOP
// winning first cannot admit a request and an admitted request has an actor
// lifetime fence installed before it can be published.
//
// A dedicated external CONTROL worker observes revocations, closes owned
// request stores, polls their in-flight leases and actor DONE independently. The
// worker is never a scheduler carrier. Its progress requires scheduling and
// termination of admitted work; this is not a hard-real-time guarantee.
// Physical actor slot reuse and generation reactivation are fail-closed.
// Source .atto must NOT construct or access this registry; this is NOT guest
// ownership/type or tenant/isolate authentication.

#include "oreslang/runtime/NativeActorScheduler.h"
#include "oreslang/runtime/RequestContextLocalV1.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace oreslang::runtime {

class RequestLocalActorTerminationRegistryV1 final {
public:
    static constexpr std::size_t max_actor_slots = 256;
    static constexpr std::size_t max_requests_per_actor = 32;
    using Ptr = std::unique_ptr<RequestLocalActorTerminationRegistryV1>;

    // Bootstrap only, BEFORE any actor is spawned. Configures the one
    // scheduler observer and preallocates all actor-lifetime atomic fences.
    // Ownership order: this registry must outlive the scheduler's final
    // callback and be explicitly joined BEFORE scheduler destruction.
    static Ptr create(OresNativeActorScheduler* scheduler,
                      std::size_t actor_slots) {
        if (!scheduler || actor_slots == 0 || actor_slots > max_actor_slots ||
            actor_slots != ores_native_actor_scheduler_actor_capacity_v1(scheduler))
            return {};
        Ptr registry(new RequestLocalActorTerminationRegistryV1(
            scheduler, actor_slots));
        // Start the external worker first. If scheduler observer install
        // fails, it can be stopped without a dangling callback registration.
        registry->worker_ = std::thread(
            [owner = registry.get()] { owner->sweep_loop(); });
        if (!ores_native_actor_scheduler_configure_termination_observer_v1(
                scheduler, &on_termination, registry.get())) {
            registry->shutdown_.store(true, std::memory_order_release);
            registry->worker_.join();
            return {};
        }
        return registry;
    }

    RequestLocalActorTerminationRegistryV1(
        const RequestLocalActorTerminationRegistryV1&) = delete;
    RequestLocalActorTerminationRegistryV1& operator=(
        const RequestLocalActorTerminationRegistryV1&) = delete;

    // Fail closed rather than freeing the observer context while the
    // scheduler might still call it. The trusted owner MUST call
    // shutdown_after_retire() and join before destroying either object.
    ~RequestLocalActorTerminationRegistryV1() {
        if (worker_.joinable()) std::terminate();
    }

    bool admit(OresNativeActorRef actor,
               const RequestContextLocalV1::Ptr& request) {
        if (!request || !request->identity().valid() ||
            !request->belongs_to(actor) || !actor.scheduler_domain ||
            !actor.generation || actor.actor_slot >= actor_slots_ ||
            admissions_sealed_.load(std::memory_order_acquire)) return false;
        Slot& slot = slots_[static_cast<std::size_t>(actor.actor_slot)];
        std::lock_guard<std::mutex> guard(mutex_);
        if (admissions_sealed_.load(std::memory_order_acquire) ||
            !slot.live->load(std::memory_order_acquire) ||
            slot.count == max_requests_per_actor) return false;
        for (std::size_t i = 0; i < slot.count; ++i)
            if (slot.requests[i].get() == request.get()) return false;

        // One-shot attachment before committing admission. If STOP wins
        // between attachment and scheduler commit, its atomic observer fence
        // revokes this request and commit_if_live denies publication.
        if (!request->attach_actor_lifetime_fence_once(slot.live))
            return false;
        Admission admission{&slot, &request, actor};
        const bool committed = ores_native_actor_scheduler_commit_if_live_v1(
            scheduler_, actor, &admit_under_scheduler_lock, &admission) != 0;
        // Once attached, this request cannot be rebound to another actor,
        // even after a failed admission. This is deliberate fail-closed
        // identity handling for stale handles.
        return committed && request->active();
    }

    // Do not attach a native DATA-mail issuer to a registry installed on
    // another scheduler, even if both have equal actor slot capacities.
    // The scheduler pointer is a trusted host lifetime identity, not a
    // cross-tenant or source-language security token.
    bool belongs_to_scheduler(
            const OresNativeActorScheduler* scheduler) const noexcept {
        return scheduler_ == scheduler;
    }

    // Host ingress must never expose the raw newly opened request to guest
    // code before scheduler-serialized registry admission succeeds. A failed
    // registration closes the provisional scope rather than returning a
    // usable untracked object. The caller is a trusted transport issuer:
    // identity fields are NOT authenticated merely by this C++ API.
    RequestContextLocalV1::Ptr admit_from_trusted_ingress(
            RequestIdentityV1 identity,
            TaskCancellation::Ptr cancellation) {
        if (!cancellation || !identity.valid() ||
            admissions_sealed_.load(std::memory_order_acquire))
            return {};
        auto scope = RequestContextLocalV1::open(
            identity, std::move(cancellation));
        if (!scope) return {};
        if (!admit(identity.actor, scope)) {
            scope->close();
            return {};
        }
        return scope;
    }

    // All trusted supervisor causes use the same scheduler STOP observer.
    // Admission/termination linearize under the scheduler mutex. Returns
    // false for stale or already terminated refs; never blocks for DONE.
    // Panic/cancel/restart/hot-reload MUST abandon queued DATA. Actual
    // generation revival/migration is not implemented by this host facade.
    enum class SupervisorCause {
        normal_stop,
        cancellation,
        panic,
        supervisor_shutdown,
        restart,
        hot_reload
    };

    bool signal_supervisor_termination(
            OresNativeActorRef actor, SupervisorCause cause) {
        if (!actor.scheduler_domain || !actor.generation ||
            actor.actor_slot >= actor_slots_ ||
            admissions_sealed_.load(std::memory_order_acquire))
            return false;
        const bool graceful = cause == SupervisorCause::normal_stop;
        return graceful
            ? ores_native_actor_scheduler_stop(scheduler_, actor) != 0
            : ores_native_actor_scheduler_stop_sealed(scheduler_, actor) != 0;
    }

    // CONTROL only; never on any scheduler carrier. Returns false for an
    // active/stale/unregistered actor rather than waiting indefinitely.
    bool wait_reaped(OresNativeActorRef actor) {
        if (!actor.scheduler_domain || !actor.generation ||
            actor.actor_slot >= actor_slots_ ||
            ores_native_actor_scheduler_is_any_current_carrier_v1())
            return false;
        Slot& slot = slots_[static_cast<std::size_t>(actor.actor_slot)];
        if (slot.live->load(std::memory_order_acquire) ||
            slot.terminated_domain.load(std::memory_order_acquire)
                != actor.scheduler_domain ||
            slot.terminated_generation.load(std::memory_order_acquire)
                != actor.generation)
            return false;
        std::unique_lock<std::mutex> guard(mutex_);
        reaped_.wait(guard, [&] { return slot.sweep_done; });
        return slot.sweep_ok;
    }

    // Nonblocking status probe for deterministic sibling progress tests and
    // external CONTROL coordinators. Never waits for a request lease/DONE.
    bool poll_reaped(OresNativeActorRef actor) const {
        if (!actor.scheduler_domain || !actor.generation ||
            actor.actor_slot >= actor_slots_ ||
            ores_native_actor_scheduler_is_any_current_carrier_v1())
            return false;
        const Slot& slot = slots_[static_cast<std::size_t>(actor.actor_slot)];
        if (slot.live->load(std::memory_order_acquire) ||
            slot.terminated_domain.load(std::memory_order_acquire) !=
                actor.scheduler_domain ||
            slot.terminated_generation.load(std::memory_order_acquire) !=
                actor.generation)
            return false;
        std::lock_guard<std::mutex> guard(mutex_);
        return slot.sweep_done && slot.sweep_ok;
    }

    std::size_t reaped_actors() const noexcept {
        return reaped_count_.load(std::memory_order_acquire);
    }
    std::size_t rejected_terminations() const noexcept {
        return invalid_terminations_.load(std::memory_order_acquire);
    }

    // Privileged external CONTROL plane must own scheduler and registry
    // lifetimes, and must NOT call this from a carrier. Retires all actors
    // without waiting inside scheduler STOP, then joins external cleanup.
    // No scheduler calls can follow scheduler destruction. A stopped owner
    // may now run scheduler.drain() and scheduler.try_destroy().
    bool shutdown_after_retire() {
        if (ores_native_actor_scheduler_is_any_current_carrier_v1())
            return false;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (admissions_sealed_.load(std::memory_order_acquire))
                return false;
            // Seal the shutdown owner before retiring or joining; concurrent
            // callers may not join the same worker or retire twice.
            admissions_sealed_.store(true, std::memory_order_release);
        }
        ores_native_actor_scheduler_retire(scheduler_);
        shutdown_.store(true, std::memory_order_release);
        if (worker_.joinable()) worker_.join();
        return true;
    }

private:
    struct Slot {
        Slot() : live(std::make_shared<std::atomic<bool>>(true)) {}
        const std::shared_ptr<std::atomic<bool>> live;
        std::atomic<std::uint64_t> claimed_domain{0};
        std::atomic<std::uint64_t> claimed_generation{0};
        std::atomic<std::uint64_t> terminated_domain{0};
        std::atomic<std::uint64_t> terminated_generation{0};
        std::array<RequestContextLocalV1::Ptr, max_requests_per_actor> requests{};
        std::size_t count = 0;
        bool sweep_claimed = false;
        bool sweep_done = false;
        bool sweep_ok = false;
    };
    struct Admission {
        Slot* slot;
        const RequestContextLocalV1::Ptr* request;
        OresNativeActorRef actor;
    };

    RequestLocalActorTerminationRegistryV1(
        OresNativeActorScheduler* scheduler, std::size_t actor_slots)
        : scheduler_(scheduler), actor_slots_(actor_slots),
          slots_(new Slot[actor_slots]) {}

    // Runs under BOTH registry mutex and scheduler lifecycle mutex.
    // All storage is preallocated. No guest callback, copy constructor,
    // external lock, allocation or destructor executes in this callback.
    static int admit_under_scheduler_lock(void* opaque) noexcept {
        const auto& a = *static_cast<const Admission*>(opaque);
        Slot& slot = *a.slot;
        const auto generation =
            slot.claimed_generation.load(std::memory_order_relaxed);
        const auto domain = slot.claimed_domain.load(std::memory_order_relaxed);
        if (!slot.live->load(std::memory_order_acquire) ||
            (generation && generation != a.actor.generation) ||
            (domain && domain != a.actor.scheduler_domain) ||
            slot.count >= max_requests_per_actor)
            return 0;
        slot.claimed_generation.store(
            a.actor.generation, std::memory_order_relaxed);
        slot.claimed_domain.store(
            a.actor.scheduler_domain, std::memory_order_relaxed);
        slot.requests[slot.count] = *a.request; // empty slot; ref-count only
        ++slot.count;
        return 1;
    }

    // Callback from C while the scheduler already owns its lifecycle mutex.
    // Only C-compatible atomic operations; no wake syscalls, locking, C++
    // value destruction, scheduler reentry, or dynamic allocation.
    static void on_termination(void* opaque, OresNativeActorRef actor,
                               OresNativeActorTerminationKind) noexcept {
        auto* self =
            static_cast<RequestLocalActorTerminationRegistryV1*>(opaque);
        if (actor.actor_slot >= self->actor_slots_ ||
            !actor.scheduler_domain || !actor.generation) {
            self->invalid_terminations_.fetch_add(
                1, std::memory_order_relaxed);
            return;
        }
        Slot& slot = self->slots_[static_cast<std::size_t>(actor.actor_slot)];
        const auto claimed =
            slot.claimed_generation.load(std::memory_order_relaxed);
        const auto claimed_domain =
            slot.claimed_domain.load(std::memory_order_relaxed);
        if ((claimed && claimed != actor.generation) ||
            (claimed_domain && claimed_domain != actor.scheduler_domain))
            self->invalid_terminations_.fetch_add(
                1, std::memory_order_relaxed);
        // Even an unexpected incarnation MUST revoke access, not leave an
        // old request fence live. Never reactivate a retired slot.
        slot.terminated_domain.store(
            actor.scheduler_domain, std::memory_order_relaxed);
        slot.terminated_generation.store(
            actor.generation, std::memory_order_relaxed);
        slot.live->store(false, std::memory_order_release);
    }

    void sweep_loop() {
        for (;;) {
            bool progressed = false;
            // Independently process every revoked actor on each pass. Never
            // block waiting for one actor's pinned operation or DONE barrier:
            // one stalled actor must not starve an unrelated actor's cleanup.
            for (std::size_t i = 0; i < actor_slots_; ++i) {
                Slot& slot = slots_[i];
                if (slot.live->load(std::memory_order_acquire)) continue;
                std::array<RequestContextLocalV1::Ptr,
                           max_requests_per_actor> pinned{};
                std::size_t count = 0;
                bool issue_close = false;
                OresNativeActorRef actor{};
                {
                    std::lock_guard<std::mutex> guard(mutex_);
                    if (slot.sweep_done) continue;
                    count = slot.count;
                    actor = {slot.terminated_domain.load(
                                 std::memory_order_relaxed),
                             static_cast<std::uint64_t>(i),
                             slot.terminated_generation.load(
                                 std::memory_order_relaxed)};
                    if (!slot.sweep_claimed) {
                        slot.sweep_claimed = true;
                        issue_close = true;
                    }
                    // Copy bounded strong refs while protected by the short
                    // registry lock; never destroy request data under it.
                    for (std::size_t k = 0; k < count; ++k)
                        pinned[k] = slot.requests[k];
                }
                if (issue_close) {
                    for (std::size_t k = 0; k < count; ++k)
                        if (pinned[k]) pinned[k]->close();
                    progressed = true;
                }
                bool quiescent = true;
                for (std::size_t k = 0; k < count; ++k)
                    if (pinned[k] && !pinned[k]->closed_quiescent_now())
                        quiescent = false;
                // Nonblocking DONE query avoids head-of-line blocking while
                // a different actor has outstanding operations or wake loans.
                if (!quiescent ||
                    !ores_native_actor_scheduler_poll_done(scheduler_, actor))
                    continue;

                std::array<RequestContextLocalV1::Ptr,
                           max_requests_per_actor> released{};
                {
                    std::lock_guard<std::mutex> guard(mutex_);
                    if (slot.sweep_done) continue;
                    for (std::size_t k = 0; k < count; ++k)
                        released[k] = std::move(slot.requests[k]);
                    slot.count = 0;
                    slot.sweep_ok = true;
                    slot.sweep_done = true;
                }
                // Both released and pinned refs retire outside all locks.
                reaped_count_.fetch_add(1, std::memory_order_release);
                reaped_.notify_all();
                progressed = true;
            }

            if (shutdown_.load(std::memory_order_acquire)) {
                bool unfinished = false;
                for (std::size_t i = 0; i < actor_slots_; ++i) {
                    Slot& slot = slots_[i];
                    if (!slot.live->load(std::memory_order_acquire)) {
                        std::lock_guard<std::mutex> guard(mutex_);
                        if (!slot.sweep_done) unfinished = true;
                    }
                }
                if (!unfinished) return;
            }
            if (!progressed)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    OresNativeActorScheduler* const scheduler_;
    const std::size_t actor_slots_;
    std::unique_ptr<Slot[]> slots_;
    mutable std::mutex mutex_;
    std::condition_variable reaped_;
    std::atomic<bool> shutdown_{false};
    std::atomic<bool> admissions_sealed_{false};
    std::atomic<std::size_t> invalid_terminations_{0};
    std::atomic<std::size_t> reaped_count_{0};
    std::thread worker_;
};

} // namespace oreslang::runtime
