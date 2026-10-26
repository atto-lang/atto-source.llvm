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
    // Opaque host-only frame capability. Numeric identity is routing/debug
    // metadata, not authority: two live frames may accidentally reuse the
    // same future_id and must still reject each other's tickets.
    std::shared_ptr<const void> frame_capability;
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
        pending, settling, settling_cancelled, ready,
        scheduled, running, completed, cancelled
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
        // Fast reject an already detached/finalized incarnation before any
        // allocation or resource-accounting mutation.
        if (owner.delivery_closed() || owner.resource_admission_closed())
            throw std::logic_error("scalar future owner delivery is closed");

        // Allocate the opaque capability before taking the actor resource loan
        // so allocation failure cannot strand resource accounting.
        auto capability = std::make_shared<Capability>();

        // This CAS-backed loan is the frame-admission linearization point
        // against physical finalization. finalized() can seal resource_state
        // only from zero, while a successfully opened frame keeps it nonzero
        // until the frame terminalizes or is dropped.
        owner.retain_resource_loan();

        std::shared_ptr<NativeScalarFutureAwait> frame;
        try {
            frame = std::shared_ptr<NativeScalarFutureAwait>(
                new NativeScalarFutureAwait(owner, future_id,
                                            std::move(capability)));
        } catch (...) {
            owner.release_resource_loan();
            throw;
        }

        // Group close does not seal resource_state, so it may race after the
        // initial check and after loan admission. Reject that ordering by
        // terminalizing the just-created frame, which also releases the loan.
        if (owner.delivery_closed()) {
            (void)frame->cancel(frame->id());
            throw std::logic_error("scalar future owner delivery is closed");
        }
        return frame;
    }

    NativeScalarFutureAwait(const NativeScalarFutureAwait&) = delete;
    NativeScalarFutureAwait& operator=(const NativeScalarFutureAwait&) = delete;

    ~NativeScalarFutureAwait() {
        release_owner_loan_once();
    }

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
            if (observed == Phase::settling
                    || observed == Phase::settling_cancelled
                    || observed == Phase::running
                    || observed == Phase::completed
                    || observed == Phase::cancelled)
                return false;
            if (phase_.compare_exchange_weak(
                    observed, Phase::cancelled,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                release_owner_loan_once();
                return true;
            }
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
         * Arbitrate teardown on the same phase atomic instead of a second
         * cross-atomic cancellation flag: this gives settle-vs-detach one
         * linearization point under the C++ memory model.
         */
        Phase observed = phase_.load(std::memory_order_acquire);
        for (;;) {
            if (observed == Phase::settling) {
                if (phase_.compare_exchange_weak(
                        observed, Phase::settling_cancelled,
                        std::memory_order_acq_rel, std::memory_order_acquire))
                    return true;
                continue;
            }
            if (observed == Phase::settling_cancelled
                    || observed == Phase::cancelled)
                return true; // teardown is already reconciled or committed.
            if (observed == Phase::running || observed == Phase::completed)
                return false;
            if (phase_.compare_exchange_weak(
                    observed, Phase::cancelled,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                release_owner_loan_once();
                return true;
            }
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
        if (parent_ref.delivery_closed()
                || parent_ref.resource_admission_closed()) {
            if (parent_ref.delivery_closed())
                (void)cancel_if_owner_detached(ticket, parent_ref);
            else
                (void)cancel(ticket);
            return false;
        }

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
                        self->release_owner_loan_once();
                        throw;
                    }
                    self->phase_.store(
                        Phase::completed, std::memory_order_release);
                    self->release_owner_loan_once();
                });
        } catch (...) {
            expected = Phase::scheduled;
            const Phase fallback =
                (parent_ref.delivery_closed()
                 || parent_ref.resource_admission_closed())
                    ? Phase::cancelled : Phase::ready;
            if (phase_.compare_exchange_strong(
                    expected, fallback,
                    std::memory_order_acq_rel, std::memory_order_acquire)
                    && fallback == Phase::cancelled)
                release_owner_loan_once();
            throw;
        }

        if (!enqueued) {
            expected = Phase::scheduled;
            // If close won during CONTROL admission, do not resurrect READY.
            // Otherwise preserve the resolved value for bounded retry.
            const Phase fallback =
                (parent_ref.delivery_closed()
                 || parent_ref.resource_admission_closed())
                    ? Phase::cancelled : Phase::ready;
            if (phase_.compare_exchange_strong(
                    expected, fallback,
                    std::memory_order_acq_rel, std::memory_order_acquire)
                    && fallback == Phase::cancelled)
                release_owner_loan_once();
            return false;
        }

        // Close may race immediately after successful enqueue. Convert the
        // generation-fenced barrier into cancellation while the callback is
        // still only scheduled; the queued CONTROL then drains as a no-op.
        if (parent_ref.delivery_closed()
                || parent_ref.resource_admission_closed()) {
            expected = Phase::scheduled;
            if (phase_.compare_exchange_strong(
                    expected, Phase::cancelled,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                release_owner_loan_once();
                return false;
            }
        }
        return true;
    }

private:
    struct Capability final {};

    NativeScalarFutureAwait(const NativeActorRef& owner,
                            std::uint64_t future_id,
                            std::shared_ptr<const void> capability)
        : owner_(owner),
          id_{owner.runtime_id(), owner.actor_id(), owner.generation(), future_id,
              std::move(capability)} {}

    bool valid(const NativeScalarFutureId& ticket) const noexcept {
        return ticket.runtime_id == id_.runtime_id
            && ticket.actor_id == id_.actor_id
            && ticket.generation == id_.generation
            && ticket.future_id == id_.future_id
            && ticket.frame_capability
            && ticket.frame_capability == id_.frame_capability;
    }

    bool settle(NativeScalarFutureId ticket, Result outcome) noexcept {
        if (!valid(ticket)) return false;
        Phase expected = Phase::pending;
        if (!phase_.compare_exchange_strong(
                expected, Phase::settling,
                std::memory_order_acq_rel, std::memory_order_acquire))
            return false;
        result_ = outcome;
        expected = Phase::settling;
        if (phase_.compare_exchange_strong(
                expected, Phase::ready,
                std::memory_order_acq_rel, std::memory_order_acquire))
            return true;
        if (expected == Phase::settling_cancelled) {
            phase_.store(Phase::cancelled, std::memory_order_release);
            release_owner_loan_once();
            return true;
        }
        std::terminate();
    }

    void release_owner_loan_once() noexcept {
        bool expected = true;
        if (resource_loan_held_.compare_exchange_strong(
                expected, false,
                std::memory_order_acq_rel, std::memory_order_acquire))
            owner_.release_resource_loan();
    }

    const NativeActorRef owner_;
    const NativeScalarFutureId id_;
    std::atomic<Phase> phase_{Phase::pending};
    std::atomic<bool> resource_loan_held_{true};
    Result result_;
};

} // namespace oreslang::runtime
