#pragma once

#include "oreslang/runtime/GpuBufferFence.hpp"
#include "oreslang/runtime/NativeActorTurnCredits.hpp"
#include "oreslang/runtime/SupervisorFailureLane.hpp"

#include <cstdint>
#include <exception>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace oreslang::gpu_actor {

class GpuActorQuarantineRegistryV1;

/**
 * Native host-side GPU actor turn coordinator, v0.
 *
 * This is a compositional integration of existing BufferFence,
 * NativeActorTurnCredits and SupervisorFailureLane. The caller MUST be a
 * trusted host provider which separately verifies actual GPU submission,
 * authenticated event quiescence, allocation release and device identity.
 * There is NO device allocation, GPU dispatch, source lowering or provider
 * authentication here; this class is not guest accessible.
 *
 * Exactly one uncommitted turn per actor incarnation; completion publishes
 * staged scalar state only after the trusted host has observed the matching
 * provider event. Cancellation retains in-flight resources until the event
 * is quiescent. No implicit CPU fallback.
 */
class HostTurnBridgeV0 final {
    // The V1 supervisor registry (including its nested Entry) is the sole
    // production caller allowed to assert provider-derived lifecycle proof.
    // No ordinary native consumer can manufacture completion by passing
    // true/true into the V0 host-only Boolean seam.
    friend class GpuActorQuarantineRegistryV1;
public:
    using ActorKey = oreslang::SupervisorFailureLane::ActorKey;
    using FailureKind = oreslang::SupervisorFailureLane::Kind;

    struct Turn {
        std::uint64_t actor_generation;
        std::uint64_t sequence;
    };

    HostTurnBridgeV0(ActorKey actor, std::string parent_id,
                     oreslang::SupervisorFailureLane& control,
                     oreslang::gpu::Identity buffer_identity,
                     std::uint64_t buffer_bytes,
                     std::int64_t initial_state = 0)
        : actor_(std::move(actor)),
          parent_id_(std::move(parent_id)),
          control_(control),
          identity_(std::move(buffer_identity)),
          fence_(identity_, buffer_bytes),
          state_(initial_state) {
        if (actor_.id.empty() || actor_.generation == 0
                || identity_.allocation_generation == 0
                || identity_.tenant.empty() || identity_.device.empty()
                || parent_id_.empty()) {
            throw std::invalid_argument("GPU_ACTOR_INVALID_AUTHORITY");
        }
        control_.reserve(actor_); // reserve CONTROL before any turn admission
    }

    HostTurnBridgeV0(const HostTurnBridgeV0&) = delete;
    HostTurnBridgeV0& operator=(const HostTurnBridgeV0&) = delete;

    ~HostTurnBridgeV0() {
        std::lock_guard<std::mutex> guard(mu_);
        // Fail-stop rather than drop an unretired provider event, exclusive
        // actor turn, or live supervisor reservation. This bridge does not
        // own physical allocations and cannot free them on destruction.
        if (!closed_ || pending_) std::terminate();
    }

    // No test override. Host lifecycle proof is mediated by V1 registry.
private:
    // Returns a source-only host turn identity, NOT an authenticated GPU event.
    Turn prepare(std::int64_t increment) {
        std::lock_guard<std::mutex> guard(mu_);
        if (closed_ || close_requested_) {
            throw std::logic_error("GPU_ACTOR_CLOSED");
        }
        if (pending_) throw std::logic_error("GPU_ACTOR_TURN_INFLIGHT");
        if (turn_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("GPU_ACTOR_TURN_SEQUENCE_EXHAUSTED");
        }
        const auto claim = credits_.try_acquire();
        if (!claim) throw std::logic_error("GPU_ACTOR_TURN_CREDIT_EXHAUSTED");
        if ((increment > 0 && state_ > std::numeric_limits<std::int64_t>::max() - increment)
             || (increment < 0 && state_ < std::numeric_limits<std::int64_t>::min() - increment)) {
            (void)credits_.release(*claim);
            throw std::overflow_error("GPU_ACTOR_STAGE_OVERFLOW");
        }
        try {
            auto ticket = fence_.begin_command(identity_);
            const Turn turn{actor_.generation, ++turn_sequence_};
            pending_.emplace(Pending{turn, *claim, std::move(ticket),
                                     state_ + increment, false, false, false});
            return turn;
        } catch (...) {
            (void)credits_.release(*claim);
            throw;
        }
    }

    // Trusted host MUST enter the enqueue window BEFORE calling an external
    // provider. A close/device loss during this window may NOT pre-enqueue
    // abort the buffer until the provider has confirmed enqueue outcome.
    void begin_enqueue(Turn turn) {
        std::lock_guard<std::mutex> guard(mu_);
        Pending& p = require_pending(turn);
        if (close_requested_ || p.enqueue_started || p.submitted) {
            throw std::logic_error("GPU_ACTOR_ENQUEUE_WINDOW_INVALID");
        }
        p.enqueue_started = true;
    }

    // Trusted provider calls this ONLY after the command is durably enqueued.
    // If close won during enqueue, poison and await the physical event.
    void mark_submitted(Turn turn) {
        std::lock_guard<std::mutex> guard(mu_);
        Pending& p = require_pending(turn);
        if (!p.enqueue_started || p.submitted) {
            throw std::logic_error("GPU_ACTOR_SUBMISSION_INVALID");
        }
        fence_.mark_submitted(p.ticket);
        p.submitted = true;
        if (close_requested_) {
            fence_.cancel_pending(p.ticket);
            p.cancelled = true;
        }
    }

    // Trusted provider calls on a DEFINITIVE failure to enqueue (never
    // after the command could have been submitted). Abort is not cancellation
    // of submitted work. This abort terminates the actor incarnation.
    void abort_enqueue(Turn turn) {
        std::lock_guard<std::mutex> guard(mu_);
        Pending& p = require_pending(turn);
        if (!p.enqueue_started || p.submitted) {
            throw std::logic_error("GPU_ACTOR_ENQUEUE_ABORT_INVALID");
        }
        fence_.abort_unsubmitted(p.ticket);
        release_pending();
        close_requested_ = true;
        closed_ = true;
        credits_.retire();
        publish_terminal_failure("gpu actor kernel enqueue failed");
    }

    // Trusted provider must verify event identity AND device quiescence.
    // The booleans here are an untrusted-testable HOST ADAPTER seam, not proof.
    void finish_after_provider_event(Turn turn, bool successful,
                                     bool provider_event_quiescent) {
        std::lock_guard<std::mutex> guard(mu_);
        Pending& p = require_pending(turn);
        if (!p.submitted || close_requested_) throw std::logic_error("GPU_ACTOR_FINISH_INVALID");
        if (!provider_event_quiescent) throw std::logic_error("GPU_ACTOR_DEVICE_NOT_QUIESCENT");
        fence_.complete(p.ticket, successful);
        if (successful) {
            state_ = p.staged_state;
        } else {
            closed_ = true;
            publish_failure(FailureKind::HostFailure, "gpu kernel failed");
        }
        release_pending();
    }

public:
    // An actor close request never assumes an enqueued GPU kernel is cancelled.
    // An in-progress enqueue must resolve first: no premature buffer reuse.
    bool request_close() { return request_stop(StopCause::Cancel); }

    // Device loss is cohort-wide; never report actor-local device isolation.
    // Outstanding allocations remain quarantined pending provider recovery.
    bool report_device_loss() { return request_stop(StopCause::DeviceLoss); }

    // All completion and release acknowledgments originate from the
    // supervisor-owned registry, never direct tests or actor-facing code.
private:
    // Called after the *same* provider event completes/quiesces, not merely
    // because cancellation was requested or a timeout occurred.
    void acknowledge_cancelled_provider_event(Turn turn, bool provider_event_quiescent) {
        std::lock_guard<std::mutex> guard(mu_);
        Pending& p = require_pending(turn);
        if (!close_requested_ || !p.cancelled || !p.submitted) {
            throw std::logic_error("GPU_ACTOR_CANCEL_ACK_INVALID");
        }
        if (!provider_event_quiescent) throw std::logic_error("GPU_ACTOR_DEVICE_NOT_QUIESCENT");
        fence_.acknowledge_cancelled_device_event(p.ticket, identity_);
        release_pending();
        closed_ = true;
        publish_terminal_failure("gpu actor stopped after device fence");
    }

    // The physical provider must FIRST release the buffer and prove that the
    // device has stopped touching it. This call changes host ledger state only.
    void acknowledge_backend_buffer_release(bool provider_released) {
        std::lock_guard<std::mutex> guard(mu_);
        if (!provider_released || !closed_ || pending_) {
            throw std::logic_error("GPU_ACTOR_BUFFER_RELEASE_UNPROVEN");
        }
        fence_.retire_after_backend_release(identity_);
    }

    // For normal actor termination. Physical resources must already have
    // been released by a trusted provider, and no command may remain.
    // Unlike cancellation, this does NOT send a failure to supervisor CONTROL.
    void retire_gracefully_after_backend_release(bool provider_released) {
        std::lock_guard<std::mutex> guard(mu_);
        if (!provider_released || closed_ || close_requested_ || pending_) {
            throw std::logic_error("GPU_ACTOR_GRACEFUL_RETIRE_UNPROVEN");
        }
        fence_.retire_after_backend_release(identity_);
        credits_.retire();
        control_.retire_gracefully(actor_);
        closed_ = true;
    }

public:
    std::int64_t committed_state() const {
        std::lock_guard<std::mutex> guard(mu_);
        return state_;
    }
    bool has_pending_turn() const {
        std::lock_guard<std::mutex> guard(mu_);
        return pending_.has_value();
    }
    bool closed() const {
        std::lock_guard<std::mutex> guard(mu_);
        return closed_;
    }
    oreslang::gpu::Status fence_status() const { return fence_.status(); }

private:
    enum class StopCause { None, Cancel, DeviceLoss };

    struct Pending {
        Turn turn;
        oreslang::runtime::NativeActorTurnCredits::Claim credit;
        oreslang::gpu::Ticket ticket;
        std::int64_t staged_state;
        bool enqueue_started;
        bool submitted;
        bool cancelled;
    };

    Pending& require_pending(Turn turn) {
        if (!pending_ || pending_->turn.actor_generation != turn.actor_generation
                || pending_->turn.sequence != turn.sequence
                || turn.actor_generation != actor_.generation) {
            throw std::logic_error("GPU_ACTOR_STALE_TURN");
        }
        return *pending_;
    }

    void release_pending() {
        // Pending is not retired until the fence transitions successfully.
        const auto credit = pending_->credit;
        pending_.reset();
        if (!credits_.release(credit)) throw std::logic_error("GPU_ACTOR_CREDIT_CORRUPT");
    }

    bool request_stop(StopCause cause) {
        std::lock_guard<std::mutex> guard(mu_);
        if (closed_ || close_requested_) return false;
        close_requested_ = true;
        stop_cause_ = cause;
        credits_.retire();
        if (!pending_) {
            closed_ = true;
            publish_terminal_failure("gpu actor stopped without active command");
            return true;
        }
        if (pending_->enqueue_started && !pending_->submitted) {
            // Enqueue has entered the provider. Never infer that it failed.
            return true;
        }
        if (!pending_->submitted) {
            fence_.abort_unsubmitted(pending_->ticket);
            release_pending();
            closed_ = true;
            publish_terminal_failure("gpu actor stopped before enqueue");
        } else {
            fence_.cancel_pending(pending_->ticket);
            pending_->cancelled = true;
        }
        return true;
    }

    void publish_terminal_failure(const char* message) {
        publish_failure(stop_cause_ == StopCause::DeviceLoss
                || stop_cause_ == StopCause::None
                ? FailureKind::HostFailure : FailureKind::Cancel, message);
    }

    void publish_failure(FailureKind kind, const char* message) {
        if (!control_.publish_preallocated(actor_, kind, message, parent_id_)) {
            throw std::logic_error("GPU_ACTOR_CONTROL_DUPLICATE");
        }
    }

    ActorKey actor_;
    std::string parent_id_;
    oreslang::SupervisorFailureLane& control_;
    oreslang::gpu::Identity identity_;
    oreslang::gpu::BufferFence fence_;
    oreslang::runtime::NativeActorTurnCredits credits_{1};
    mutable std::mutex mu_;
    std::int64_t state_;
    std::uint64_t turn_sequence_ = 0;
    std::optional<Pending> pending_;
    StopCause stop_cause_ = StopCause::None;
    bool close_requested_ = false;
    bool closed_ = false;
};

} // namespace oreslang::gpu_actor
