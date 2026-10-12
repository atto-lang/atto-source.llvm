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
struct Ticket {
    std::uint64_t generation;
    std::uint64_t sequence;
};

class BufferFence {
    struct State {
        Identity identity;
        std::uint64_t bytes;
        std::uint64_t sequence = 0;
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
    BufferFence(BufferFence&&) noexcept = default;
    BufferFence& operator=(BufferFence&&) noexcept = default;

    BufferFence take() {
        active();
        std::lock_guard<std::mutex> guard(state_->lock);
        if (state_->status != Status::Idle)
            throw std::logic_error("cannot move device buffer with pending fence");
        return BufferFence(std::move(state_));
    }

    Ticket begin_command(const Identity& caller) {
        active();
        std::lock_guard<std::mutex> guard(state_->lock);
        if (!(state_->identity == caller)) throw std::logic_error("foreign device identity");
        if (state_->status != Status::Idle) throw std::logic_error("buffer not idle");
        if (state_->sequence == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("device ticket sequence exhausted");
        state_->backend_quiescent = false;
        state_->status = Status::InFlight;
        return Ticket{state_->identity.allocation_generation, ++state_->sequence};
    }

    void complete(Ticket ticket, bool succeeded) {
        active();
        std::lock_guard<std::mutex> guard(state_->lock);
        validate_inflight(ticket);
        state_->status = succeeded ? Status::Idle : Status::Poisoned;
        state_->backend_quiescent = true;
    }
    void cancel_pending(Ticket ticket) {
        active();
        std::lock_guard<std::mutex> guard(state_->lock);
        validate_inflight(ticket);
        state_->status = Status::Poisoned;
    }
    void acknowledge_cancelled_device_event(Ticket ticket, const Identity& caller) {
        active();
        std::lock_guard<std::mutex> guard(state_->lock);
        if (!(state_->identity == caller) || state_->status != Status::Poisoned
            || state_->backend_quiescent
            || ticket.generation != state_->identity.allocation_generation
            || ticket.sequence != state_->sequence)
            throw std::logic_error("stale cancelled device fence acknowledgement");
        state_->backend_quiescent = true;
    }
    void retire_after_backend_release(const Identity& caller) {
        active();
        std::lock_guard<std::mutex> guard(state_->lock);
        if (!(state_->identity == caller)) throw std::logic_error("foreign device identity");
        if (state_->status == Status::InFlight || !state_->backend_quiescent)
            throw std::logic_error("device command not yet quiescent");
        state_->status = Status::Closed;
    }
    void close_after_retirement() const {
        if (!state_) return;
        std::lock_guard<std::mutex> guard(state_->lock);
        if (state_->status != Status::Closed)
            throw std::logic_error("backend must release device buffer first");
    }
    Status status() const {
        active();
        std::lock_guard<std::mutex> guard(state_->lock);
        return state_->status;
    }
    // A per-allocation metadata snapshot, not provider authorization.
    struct Snapshot { Identity identity; std::uint64_t bytes; Status status; };
    Snapshot snapshot() const {
        active();
        std::lock_guard<std::mutex> guard(state_->lock);
        return Snapshot{state_->identity, state_->bytes, state_->status};
    }

private:
    explicit BufferFence(std::shared_ptr<State> state) : state_(std::move(state)) {}
    void active() const {
        if (!state_) throw std::logic_error("device ownership moved");
        std::lock_guard<std::mutex> guard(state_->lock);
        if (state_->status == Status::Closed) throw std::logic_error("device buffer closed");
    }
    void validate_inflight(Ticket ticket) const {
        if (state_->status != Status::InFlight ||
            ticket.generation != state_->identity.allocation_generation ||
            ticket.sequence != state_->sequence)
            throw std::logic_error("stale or mismatched device event");
    }
    std::shared_ptr<State> state_;
};

} // namespace oreslang::gpu
