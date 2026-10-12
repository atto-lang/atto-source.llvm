#pragma once

#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

namespace oreslang::gpu {

// Host-side descriptor/fence ledger ONLY. It does not allocate device memory,
// prove provider credentials or authorize dispatch; an external device backend
// must own the actual opaque handle and confirm quiescent physical release.
enum class Status { Idle, InFlight, Poisoned, Closed };
struct Identity {
    std::string tenant;
    std::string device;
    std::uint64_t allocation_generation;
    bool operator==(const Identity& other) const noexcept {
        return tenant == other.tenant && device == other.device
            && allocation_generation == other.allocation_generation;
    }
};
class BufferFence;
class Ticket {
public:
    std::uint64_t generation() const noexcept { return generation_; }
    std::uint64_t sequence() const noexcept { return sequence_; }

private:
    Ticket(std::uint64_t generation, std::uint64_t sequence,
           std::shared_ptr<const std::uint8_t> proof) noexcept
        : generation_(generation), sequence_(sequence), proof_(std::move(proof)) {}

    std::uint64_t generation_;
    std::uint64_t sequence_;
    // A unique, unexposed per-submission identity. Kept alive by the ticket;
    // numeric generation + sequence alone must never authorize completion.
    std::shared_ptr<const std::uint8_t> proof_;
    friend class BufferFence;
};

class BufferFence {
    struct State {
        Identity identity;
        std::uint64_t bytes;
        std::uint64_t sequence = 0;
        std::shared_ptr<const std::uint8_t> active_ticket_proof;
        Status status = Status::Idle;
        bool backend_quiescent = true;
        mutable std::mutex lock;
        State(Identity i, std::uint64_t n)
            : identity(std::move(i)), bytes(n) {
            if (identity.tenant.empty() || identity.device.empty() || bytes == 0)
                throw std::invalid_argument("invalid device identity or byte limit");
        }
    };
public:
    BufferFence(Identity identity, std::uint64_t bytes)
        : state_(std::make_shared<State>(std::move(identity), bytes)) {}
    BufferFence(const BufferFence&) = delete;
    BufferFence& operator=(const BufferFence&) = delete;
    // The moved-from owner must never race with a method dereferencing
    // state_. All wrapper operations take owner_lock_ before State::lock.
    BufferFence(BufferFence&& other) {
        std::lock_guard<std::mutex> owner(other.owner_lock_);
        // A direct std::move must not bypass take()'s in-flight fence.
        if (other.state_) {
            std::lock_guard<std::mutex> guard(other.state_->lock);
            if (other.state_->status != Status::Idle)
                throw std::logic_error("cannot move device buffer with pending fence");
        }
        state_ = std::move(other.state_);
    }
    BufferFence& operator=(BufferFence&& other) {
        if (this == &other) return *this;
        std::scoped_lock owners(owner_lock_, other.owner_lock_);
        // Assignment must not silently abandon an unretired device handle.
        if (state_) {
            std::lock_guard<std::mutex> state_guard(state_->lock);
            if (state_->status != Status::Closed)
                throw std::logic_error("cannot overwrite unretired device buffer");
        }
        // Move assignment is an alternative ownership-transfer entrypoint;
        // validate the source too, before mutating either wrapper.
        if (other.state_) {
            std::lock_guard<std::mutex> guard(other.state_->lock);
            if (other.state_->status != Status::Idle)
                throw std::logic_error("cannot move device buffer with pending fence");
        }
        state_ = std::move(other.state_);
        return *this;
    }

    BufferFence take() {
        std::lock_guard<std::mutex> owner(owner_lock_);
        active();
        std::lock_guard<std::mutex> guard(state_->lock);
        if (state_->status != Status::Idle)
            throw std::logic_error("cannot move device buffer with pending fence");
        return BufferFence(std::move(state_));
    }

    Ticket begin_command(const Identity& caller) {
        std::lock_guard<std::mutex> owner(owner_lock_);
        active();
        std::lock_guard<std::mutex> guard(state_->lock);
        if (!(state_->identity == caller)) throw std::logic_error("foreign device identity");
        if (state_->status != Status::Idle) throw std::logic_error("buffer not idle");
        if (state_->sequence == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("device ticket sequence exhausted");
        // Allocate the identity before changing state so allocation failure
        // cannot leave a submitted command without a usable ticket.
        auto proof = std::make_shared<const std::uint8_t>(0);
        const auto next = state_->sequence + 1;
        Ticket ticket{state_->identity.allocation_generation, next, proof};
        state_->active_ticket_proof = std::move(proof);
        state_->sequence = next;
        state_->backend_quiescent = false;
        state_->status = Status::InFlight;
        return ticket;
    }

    void complete(Ticket ticket, bool succeeded) {
        std::lock_guard<std::mutex> owner(owner_lock_);
        active();
        std::lock_guard<std::mutex> guard(state_->lock);
        validate_inflight(ticket);
        state_->status = succeeded ? Status::Idle : Status::Poisoned;
        state_->backend_quiescent = true;
        state_->active_ticket_proof.reset();
    }
    void cancel_pending(Ticket ticket) {
        std::lock_guard<std::mutex> owner(owner_lock_);
        active();
        std::lock_guard<std::mutex> guard(state_->lock);
        validate_inflight(ticket);
        state_->status = Status::Poisoned;
    }
    void acknowledge_cancelled_device_event(Ticket ticket, const Identity& caller) {
        std::lock_guard<std::mutex> owner(owner_lock_);
        active();
        std::lock_guard<std::mutex> guard(state_->lock);
        if (!(state_->identity == caller) || state_->status != Status::Poisoned
            || state_->backend_quiescent
            || ticket.generation_ != state_->identity.allocation_generation
            || ticket.sequence_ != state_->sequence
            || !ticket.proof_
            || ticket.proof_ != state_->active_ticket_proof)
            throw std::logic_error("stale cancelled device fence acknowledgement");
        state_->backend_quiescent = true;
        state_->active_ticket_proof.reset();
    }
    void retire_after_backend_release(const Identity& caller) {
        std::lock_guard<std::mutex> owner(owner_lock_);
        active();
        std::lock_guard<std::mutex> guard(state_->lock);
        if (!(state_->identity == caller)) throw std::logic_error("foreign device identity");
        if (state_->status == Status::InFlight || !state_->backend_quiescent)
            throw std::logic_error("device command not yet quiescent");
        state_->status = Status::Closed;
    }
    void close_after_retirement() const {
        std::lock_guard<std::mutex> owner(owner_lock_);
        if (!state_) return;
        std::lock_guard<std::mutex> guard(state_->lock);
        if (state_->status != Status::Closed)
            throw std::logic_error("backend must release device buffer first");
    }
    Status status() const {
        std::lock_guard<std::mutex> owner(owner_lock_);
        active();
        std::lock_guard<std::mutex> guard(state_->lock);
        return state_->status;
    }

private:
    explicit BufferFence(std::shared_ptr<State> state) : state_(std::move(state)) {}
    // Caller must already hold owner_lock_ to make the state_ access safe.
    void active() const {
        if (!state_) throw std::logic_error("device ownership moved");
        std::lock_guard<std::mutex> guard(state_->lock);
        if (state_->status == Status::Closed) throw std::logic_error("device buffer closed");
    }
    void validate_inflight(Ticket ticket) const {
        if (state_->status != Status::InFlight ||
            ticket.generation_ != state_->identity.allocation_generation ||
            ticket.sequence_ != state_->sequence ||
            !ticket.proof_ || ticket.proof_ != state_->active_ticket_proof)
            throw std::logic_error("stale or mismatched device event");
    }
    // Protects the *wrapper handle* (not just the shared device state).
    // Lock order: owner_lock_ -> State::lock throughout this class.
    mutable std::mutex owner_lock_;
    std::shared_ptr<State> state_;
};

} // namespace oreslang::gpu
