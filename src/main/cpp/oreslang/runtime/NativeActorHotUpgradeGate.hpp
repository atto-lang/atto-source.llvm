#pragma once

#include "oreslang/runtime/ActorHeapIdentity.hpp"
#include "oreslang/runtime/NativeSupervisorCodeSlot.hpp"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace oreslang::runtime {

/**
 * Host CONTROL-only gate for *code-only* actor upgrades.
 *
 * This is a conservative native substrate, not a guest-accessible source API.
 * Source actor lowering must attach a Pin to EVERY live turn, suspended frame,
 * async callback and FFI entry before a supervisor may rely on quiescence.
 * Heap identity and shape are immutable through code-only upgrades; changing
 * state layout, capabilities, actor kind or shared-cell ABI requires a separate
 * typed migration protocol that is intentionally not provided here.
 */
template<class CodeImage>
class NativeActorHotUpgradeGate final {
public:
    enum class Kind : std::uint8_t { trusted_shared, trusted_private, untrusted };
    struct Shape final {
        Kind kind;
        std::string message_abi;
        std::string state_schema;
        std::string shared_cell_abi;
        std::string capability_abi;

        bool operator==(const Shape& b) const noexcept {
            return kind == b.kind && message_abi == b.message_abi
                && state_schema == b.state_schema
                && shared_cell_abi == b.shared_cell_abi
                && capability_abi == b.capability_abi;
        }
    };
    using CodeSlot = NativeSupervisorCodeSlot<CodeImage>;
    using Descriptor = typename CodeSlot::Descriptor;

private:
    struct State final {
        const ActorHeapIdentity heap;
        const Shape shape;
        std::shared_ptr<CodeSlot> code;
        std::mutex mutex;
        std::condition_variable cv;
        std::uint64_t ticket_epoch = 0;
        std::size_t users = 0;
        bool frozen = false;
        bool publishing = false;
        // Last CONTROL ticket was abandoned during a publication. If that
        // publication throws, its owner must release the orphaned freeze.
        bool abandon_pending = false;
        bool closed = false;

        State(ActorHeapIdentity identity, Shape actor_shape,
              std::shared_ptr<CodeSlot> revisions)
            : heap(identity), shape(std::move(actor_shape)), code(std::move(revisions)) {}
    };
    std::shared_ptr<State> state_;

    static void validate(ActorHeapIdentity heap, const Shape& shape) {
        if (!heap.runtime || !heap.actor || !heap.epoch
            || shape.message_abi.empty() || shape.state_schema.empty()
            || shape.shared_cell_abi.empty() || shape.capability_abi.empty())
            throw std::invalid_argument("invalid native actor hot-upgrade identity or ABI");
        if (shape.message_abi.size() > 4096 || shape.state_schema.size() > 4096
            || shape.shared_cell_abi.size() > 4096 || shape.capability_abi.size() > 4096)
            throw std::length_error("native actor hot-upgrade ABI exceeds admission limit");
    }

    // Exact length-delimited ABI encoding; concatenations cannot collide.
    static std::string fingerprint(const Shape& s) {
        std::string out = "ores-actor-code-v1:";
        out += std::to_string(static_cast<unsigned>(s.kind)) + ":";
        for (const auto* x : {&s.message_abi, &s.state_schema,
                              &s.shared_cell_abi, &s.capability_abi}) {
            out += std::to_string(x->size()) + ":" + *x;
        }
        return out;
    }

public:
    class Pin final {
        friend class NativeActorHotUpgradeGate;
        std::shared_ptr<State> state_;
        typename CodeSlot::Lease code_;

        Pin(std::shared_ptr<State> state, typename CodeSlot::Lease code)
            : state_(std::move(state)), code_(std::move(code)) {}
    public:
        Pin(const Pin&) = delete;
        Pin& operator=(const Pin&) = delete;
        Pin(Pin&& other) noexcept
            : state_(std::move(other.state_)), code_(std::move(other.code_)) {}
        Pin& operator=(Pin&& other) noexcept {
            if (this != &other) {
                close();
                state_ = std::move(other.state_);
                code_ = std::move(other.code_);
            }
            return *this;
        }
        ~Pin() { close(); }
        const CodeImage& code() const { return code_.code(); }
        Descriptor descriptor() const { return code_.descriptor(); }
        void close() noexcept {
            auto state = std::move(state_);
            if (!state) return;
            // Reclaim callbacks may re-enter supervisor read APIs. Never
            // release the last code image reference under the actor gate lock.
            code_.close();
            std::lock_guard<std::mutex> lock(state->mutex);
            --state->users;
            state->cv.notify_all();
        }
    };

    class Ticket final {
        friend class NativeActorHotUpgradeGate;
        std::shared_ptr<State> state_;
        std::uint64_t epoch_;
        std::uint64_t expected_revision_;
        Ticket(std::shared_ptr<State> state, std::uint64_t epoch,
               std::uint64_t version)
            : state_(std::move(state)), epoch_(epoch), expected_revision_(version) {}
    public:
        Ticket(const Ticket&) = delete;
        Ticket& operator=(const Ticket&) = delete;
        Ticket(Ticket&& other) noexcept
            : state_(std::move(other.state_)), epoch_(other.epoch_),
              expected_revision_(other.expected_revision_) {}
        Ticket& operator=(Ticket&& other) noexcept {
            if (this != &other) {
                release();
                state_ = std::move(other.state_);
                epoch_ = other.epoch_;
                expected_revision_ = other.expected_revision_;
            }
            return *this;
        }
        ~Ticket() noexcept { release(); }
        // CONTROL may explicitly relinquish an in-flight ticket without
        // destroying the object still referenced by promote(). Publication
        // either completes or restores DATA admission on failure.
        void abandon() noexcept { release(); }
    private:
        void release() noexcept {
            auto state = std::move(state_);
            if (!state) return;
            std::lock_guard<std::mutex> lock(state->mutex);
            // Unwind / abandoned upgrade must never seal actor DATA forever.
            // Do not race an in-progress publication or override terminal close.
            if (!state->closed && state->frozen
                && state->ticket_epoch == epoch_) {
                if (state->publishing) {
                    // The publisher still owns the commit decision. Defer
                    // freeze cleanup to its success/failure finalizer.
                    state->abandon_pending = true;
                } else {
                    state->frozen = false;
                    state->cv.notify_all();
                }
            }
        }
    };

private:
    void require_ticket_locked(const Ticket& ticket) const {
        if (ticket.state_.get() != state_.get()
            || ticket.epoch_ != state_->ticket_epoch
            || !state_->frozen)
            throw std::logic_error("stale or foreign actor hot-upgrade ticket");
    }

public:
    NativeActorHotUpgradeGate(ActorHeapIdentity heap, Shape shape,
                              CodeImage initial, std::function<void()> reclaim,
                              std::size_t maximum_revisions = 8) {
        validate(heap, shape);
        // The code image is immutable once published. Neither the code slot
        // nor this gate confers guest authority over the actor's heap.
        auto revisions = std::make_shared<CodeSlot>(
            fingerprint(shape), std::move(initial), std::move(reclaim), maximum_revisions);
        state_ = std::make_shared<State>(heap, std::move(shape), std::move(revisions));
    }
    NativeActorHotUpgradeGate(const NativeActorHotUpgradeGate&) = delete;
    NativeActorHotUpgradeGate& operator=(const NativeActorHotUpgradeGate&) = delete;
    ~NativeActorHotUpgradeGate() { close(); }

    ActorHeapIdentity identity() const noexcept { return state_->heap; }

    Pin pin(ActorHeapIdentity caller) {
        auto state = state_;
        std::lock_guard<std::mutex> lock(state->mutex);
        if (caller != state->heap)
            throw std::logic_error("foreign or stale actor heap incarnation");
        if (state->closed || state->frozen)
            throw std::logic_error("native actor hot-upgrade admission sealed");
        auto code = state->code->pin();
        if (state->users == std::numeric_limits<std::size_t>::max())
            throw std::overflow_error("actor code pin count exhausted");
        ++state->users;
        return Pin(state, std::move(code));
    }

    Ticket freeze(ActorHeapIdentity caller, std::uint64_t expected_revision) {
        auto state = state_;
        std::lock_guard<std::mutex> lock(state->mutex);
        if (caller != state->heap)
            throw std::logic_error("foreign or stale actor heap incarnation");
        if (state->closed || state->frozen)
            throw std::logic_error("actor code upgrade already frozen or closed");
        if (state->code->current().revision != expected_revision)
            throw std::logic_error("actor code upgrade stale revision");
        if (state->ticket_epoch == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("actor upgrade token sequence exhausted");
        state->frozen = true;
        state->abandon_pending = false;
        return Ticket(state, ++state->ticket_epoch, expected_revision);
    }

    // A suspended continuation/FFI lease is a live user, even if its carrier
    // returned to the scheduler. The release manager should poll this outside
    // guest execution; it must not wait inside an actor DATA turn.
    bool quiesced(const Ticket& ticket) const {
        auto state = state_;
        std::lock_guard<std::mutex> lock(state->mutex);
        require_ticket_locked(ticket);
        return state->users == 0 && !state->publishing;
    }

    void abort(const Ticket& ticket) {
        auto state = state_;
        std::lock_guard<std::mutex> lock(state->mutex);
        require_ticket_locked(ticket);
        if (state->publishing)
            throw std::logic_error("cannot abort an in-progress actor code publication");
        if (state->closed)
            throw std::logic_error("actor hot-upgrade gate is closed");
        state->frozen = false;
        state->abandon_pending = false;
    }

    // Code-only publication: any ABI/shape drift, outstanding pin or stale
    // ticket fails closed. Caller retains old generation on failed publication
    // and may abort the frozen release. No state migration is performed here.
    Descriptor promote(const Ticket& ticket, Shape candidate_shape,
                       CodeImage candidate_code, std::function<void()> reclaim) {
        // Own rejection cleanup before validating. The destructor executes
        // outside the actor gate lock, including on stale/foreign attempts.
        struct Candidate final {
            struct Cleanup {
                std::function<void()> callback;
                explicit Cleanup(std::function<void()> action)
                    : callback(std::move(action)) {}
                ~Cleanup() noexcept {
                    try { if (callback) callback(); } catch (...) {}
                }
            };
            Cleanup cleanup; // Destroy last: unload only after code destructor.
            CodeImage code;
            Candidate(CodeImage image, std::function<void()> action)
                : cleanup(std::move(action)), code(std::move(image)) {}
            Candidate(const Candidate&) = delete;
            Candidate& operator=(const Candidate&) = delete;
        };
        // Stack-owned RAII avoids a heap allocation before this gate owns
        // the reclaim callback. The admitted CodeImage is noexcept movable.
        Candidate candidate(std::move(candidate_code), std::move(reclaim));
        auto state = state_;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            require_ticket_locked(ticket);
            if (state->closed)
                throw std::logic_error("actor hot-upgrade gate is closed");
            if (state->publishing || state->users != 0)
                throw std::logic_error("actor code upgrade requires quiescence");
            validate(state->heap, candidate_shape);
            if (!(candidate_shape == state->shape))
                throw std::invalid_argument("actor code/state/capability ABI migration required");
            state->publishing = true;
        }

        Descriptor published;
        try {
            // The underlying code-slot owns the callback after this call.
            // Reclamation callbacks are host-only and must enqueue cleanup.
            // Compute potentially allocating ABI material before transferring
            // either code or cleanup: argument evaluation order is not a
            // portable ownership protocol.
            auto abi = fingerprint(candidate_shape);
            published = state->code->activate(
                ticket.expected_revision_, std::move(abi),
                std::move(candidate.code), std::move(candidate.cleanup.callback));
        } catch (...) {
            bool close_slot;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->publishing = false;
                close_slot = state->closed;
                if (state->abandon_pending && !close_slot)
                    state->frozen = false;
                state->abandon_pending = false;
                state->cv.notify_all();
            }
            // Cleanup callback can call close() while publication is active.
            // The pending closer never waits or runs guest work in its callback.
            if (close_slot) state->code->close();
            throw;
        }
        bool close_slot;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->publishing = false;
            close_slot = state->closed;
            if (!close_slot) state->frozen = false;
            state->abandon_pending = false;
            state->cv.notify_all();
        }
        // If a retiring old image requested terminal close from its
        // reclaim callback, honor that request after publication finishes.
        // Do not re-open mailbox admission for a terminally closed actor.
        if (close_slot) state->code->close();
        return published;
    }

    void close() noexcept {
        auto state = state_;
        bool close_slot;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->closed) return;
            state->closed = true;
            state->frozen = true;
            // Never wait for publication: a generation reclaim callback
            // may synchronously invoke close() *from* activate(). Waiting
            // here deadlocks the actor's last publisher. The publisher
            // completes the pending physical close outside both locks.
            close_slot = !state->publishing;
        }
        if (close_slot) state->code->close();
    }
};

} // namespace oreslang::runtime
