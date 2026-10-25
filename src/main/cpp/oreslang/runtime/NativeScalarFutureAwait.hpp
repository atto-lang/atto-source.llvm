#pragma once
// Host-only scalar Future<T> precursor: guest lowering is not enabled.
// Result storage is a pinned shared allocation, never an actor heap pointer.
#include "oreslang/runtime/ActorGroupLifecycle.hpp"
#include <atomic>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <stdexcept>
#include <utility>

namespace oreslang::runtime {

struct NativeScalarFutureId {
    std::uint64_t runtime_id;
    std::uint64_t actor_id;
    std::uint64_t generation;
    std::uint64_t future_id;
};

// No waiting on a carrier, no guest callback on a producer thread. The only
// executor of Resume is NativeActorGroup's single-flight parent CONTROL lane.
// The producer MUST retain a shared/weak frame reference until its callback
// quiesces. The owning supervisor MUST reconcile group teardown: a dropped
// queued CONTROL event is exposed by NativeActorRef::control_delivery_lost()
// and may be generation-fenced into cancellation via cancel_if_owner_detached().
// This is not source Future<T> / await lowering.
class NativeScalarFutureAwait final
        : public std::enable_shared_from_this<NativeScalarFutureAwait> {
public:
    enum class Phase {
        pending, settling, ready, scheduled, running, completed, cancelled
    };
    struct Result {
        bool success = false;
        std::int64_t value = 0;
        std::int64_t error_code = 0;
    };
    using Resume = std::function<void(const Result&)>;

    static std::shared_ptr<NativeScalarFutureAwait> open(
            const NativeActorRef& owner, std::uint64_t future_id) {
        if (!owner.runtime_id() || !owner.actor_id() || !owner.generation()
                || !future_id)
            throw std::invalid_argument("invalid scalar future owner identity");
        return std::shared_ptr<NativeScalarFutureAwait>(
            new NativeScalarFutureAwait(owner, future_id));
    }

    NativeScalarFutureAwait(const NativeScalarFutureAwait&) = delete;
    NativeScalarFutureAwait& operator=(const NativeScalarFutureAwait&) = delete;

    NativeScalarFutureId id() const noexcept { return id_; }

    bool complete(NativeScalarFutureId ticket, std::int64_t value) noexcept {
        return settle(ticket, Result{true, value, 0});
    }

    bool fail(NativeScalarFutureId ticket, std::int64_t error_code) noexcept {
        if (error_code <= 0) return false;
        return settle(ticket, Result{false, 0, error_code});
    }

    // Pending/ready/queued cancellation suppresses any later CONTROL callback.
    // Once the callback has started, it is completed and cannot be revoked.
    bool cancel(NativeScalarFutureId ticket) noexcept {
        if (!valid(ticket)) return false;
        Phase observed = phase_.load(std::memory_order_acquire);
        for (;;) {
            if (observed == Phase::settling || observed == Phase::running
                    || observed == Phase::completed
                    || observed == Phase::cancelled)
                return false;
            if (phase_.compare_exchange_weak(
                    observed, Phase::cancelled,
                    std::memory_order_acq_rel, std::memory_order_acquire))
                return true;
        }
    }

    /*
     * Group close publishes a generation-fenced no-new-delivery barrier even
     * when no CONTROL item happened to be queued. Convert that barrier into
     * terminal Future cancellation so pending, ready, or scheduled frames
     * cannot survive owner teardown indefinitely.
     */
    bool cancel_if_owner_detached(
            NativeScalarFutureId ticket,
            const NativeActorRef& parent_ref) noexcept {
        if (!valid(ticket)
                || !parent_ref.same_incarnation(owner_)
                || !parent_ref.delivery_closed())
            return false;

        /*
         * Detachment is authoritative even if a producer has already won the
         * PENDING->SETTLING transition but has not published READY yet.
         * Publish a deferred teardown request first, then either cancel the
         * currently visible cancellable phase or let settle() consume it.
         */
        cancel_on_settle_.store(true, std::memory_order_release);
        Phase observed = phase_.load(std::memory_order_acquire);
        for (;;) {
            if (observed == Phase::settling)
                return true; // settle() will publish CANCELLED before returning.
            if (observed == Phase::cancelled)
                return true; // teardown is already reconciled.
            if (observed == Phase::running || observed == Phase::completed)
                return false;
            if (phase_.compare_exchange_weak(
                    observed, Phase::cancelled,
                    std::memory_order_acq_rel, std::memory_order_acquire))
                return true;
        }
    }

    Phase phase() const noexcept {
        return phase_.load(std::memory_order_acquire);
    }

    // Parent scheduler only. If CONTROL is full, leave the resolved value
    // intact for a later retry. Actor IDs AND generation are authenticated.
    bool try_schedule(NativeScalarFutureId ticket,
                      NativeActorGroup& parent_group,
                      const NativeActorRef& parent_ref,
                      Resume resume) {
        if (!resume) throw std::invalid_argument("missing scalar future resume");
        if (!valid(ticket) || !parent_ref.same_incarnation(owner_))
            return false;

        auto self = shared_from_this();
        Phase expected = Phase::ready;
        if (!phase_.compare_exchange_strong(
                expected, Phase::scheduled,
                std::memory_order_acq_rel, std::memory_order_acquire))
            return false;

        bool enqueued = false;
        try {
            enqueued = parent_group.enqueue_control(parent_ref,
                [self, resume = std::move(resume)] {
                    Phase expected = Phase::scheduled;
                    if (!self->phase_.compare_exchange_strong(
                            expected, Phase::running,
                            std::memory_order_acq_rel,
                            std::memory_order_acquire))
                        return;
                    // The winning settle transition published result_ before
                    // READY with release semantics; the READY->SCHEDULED and
                    // SCHEDULED->RUNNING acquire/release chain makes it visible.
                    const Result outcome = self->result_;
                    try {
                        resume(outcome);
                    } catch (...) {
                        self->phase_.store(
                            Phase::completed, std::memory_order_release);
                        throw;
                    }
                    self->phase_.store(
                        Phase::completed, std::memory_order_release);
                });
        } catch (...) {
            expected = Phase::scheduled;
            (void)phase_.compare_exchange_strong(
                expected, Phase::ready,
                std::memory_order_acq_rel, std::memory_order_acquire);
            throw;
        }

        if (!enqueued) {
            expected = Phase::scheduled;
            // Cancellation may have won while ActorGroup admission was tried.
            (void)phase_.compare_exchange_strong(
                expected, Phase::ready,
                std::memory_order_acq_rel, std::memory_order_acquire);
            return false;
        }
        return true;
    }

private:
    NativeScalarFutureAwait(const NativeActorRef& owner, std::uint64_t future_id)
        : owner_(owner),
          id_{owner.runtime_id(), owner.actor_id(), owner.generation(), future_id} {}

    bool valid(NativeScalarFutureId ticket) const noexcept {
        return ticket.runtime_id == id_.runtime_id
            && ticket.actor_id == id_.actor_id
            && ticket.generation == id_.generation
            && ticket.future_id == id_.future_id;
    }

    bool settle(NativeScalarFutureId ticket, Result outcome) noexcept {
        if (!valid(ticket)) return false;
        Phase expected = Phase::pending;
        if (!phase_.compare_exchange_strong(
                expected, Phase::settling,
                std::memory_order_acq_rel, std::memory_order_acquire))
            return false;
        result_ = outcome;
        phase_.store(Phase::ready, std::memory_order_release);
        if (cancel_on_settle_.load(std::memory_order_acquire)) {
            expected = Phase::ready;
            (void)phase_.compare_exchange_strong(
                expected, Phase::cancelled,
                std::memory_order_acq_rel, std::memory_order_acquire);
        }
        return true;
    }

    const NativeActorRef owner_;
    const NativeScalarFutureId id_;
    std::atomic<Phase> phase_{Phase::pending};
    std::atomic<bool> cancel_on_settle_{false};
    Result result_;
};

} // namespace oreslang::runtime
