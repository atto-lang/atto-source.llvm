#pragma once

#include <cstdint>
#include <functional>
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
// A ticket must be bound to one exact allocation and one exact submission.
// Generation/sequence are diagnostic values, NOT completion authority.
class Ticket {
public:
    std::uint64_t generation;
    std::uint64_t sequence;
private:
    std::shared_ptr<const std::uint8_t> seal_;
    Ticket(std::uint64_t g, std::uint64_t s, std::shared_ptr<const std::uint8_t> seal)
        : generation(g), sequence(s), seal_(std::move(seal)) {}
    friend class BufferFence;
};

class BufferFence {
    struct State {
        Identity identity;
        std::uint64_t bytes;
        std::uint64_t sequence = 0;
        Status status = Status::Idle;
        bool backend_quiescent = true;
        std::shared_ptr<const std::uint8_t> active_seal;
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
    // Moving the owner is synchronized with all in-progress operations.
    // The state mutex alone is insufficient: take() changes state_ itself.
    BufferFence(BufferFence&& other) noexcept {
        std::lock_guard<std::mutex> guard(other.owner_lock_);
        state_ = std::move(other.state_);
    }
    BufferFence& operator=(BufferFence&& other) {
        if (this == &other) return *this;
        std::scoped_lock guard(owner_lock_, other.owner_lock_);
        if (state_) {
            std::lock_guard<std::mutex> state_guard(state_->lock);
            if (state_->status != Status::Closed)
                throw std::logic_error("cannot overwrite live device buffer owner");
        }
        state_ = std::move(other.state_);
        return *this;
    }

    BufferFence take() {
        std::lock_guard<std::mutex> owner_guard(owner_lock_);
        active();
        std::lock_guard<std::mutex> guard(state_->lock);
        if (state_->status != Status::Idle)
            throw std::logic_error("cannot move device buffer with pending fence");
        return BufferFence(std::move(state_));
    }


    struct PairTickets {
        Identity input;
        Ticket input_ticket;
        Identity output;
        Ticket output_ticket;
    };

    // All-or-nothing paired claim for a trusted GPU provider.
    // Owner and allocation locks are acquired in stable address order, so
    // take(), begin_command(), cancel and retirement cannot interleave.
    static PairTickets claim_pair(BufferFence& input, BufferFence& output,
                                  std::uint64_t expected_bytes) {
        if (&input == &output || expected_bytes == 0)
            throw std::invalid_argument("two nonempty distinct GPU buffers required");
        auto* first = std::less<BufferFence*>{}(&input, &output) ? &input : &output;
        auto* second = first == &input ? &output : &input;
        std::lock_guard<std::mutex> owner_one(first->owner_lock_);
        std::lock_guard<std::mutex> owner_two(second->owner_lock_);
        if (!input.state_ || !output.state_ || input.state_ == output.state_)
            throw std::logic_error("moved or aliased device owner");
        auto* a = input.state_.get();
        auto* b = output.state_.get();
        auto* low = std::less<State*>{}(a, b) ? a : b;
        auto* high = low == a ? b : a;
        std::lock_guard<std::mutex> state_one(low->lock);
        std::lock_guard<std::mutex> state_two(high->lock);
        if (a->status != Status::Idle || b->status != Status::Idle
                || !a->backend_quiescent || !b->backend_quiescent)
            throw std::logic_error("device allocation not idle or not quiescent");
        if (a->bytes != expected_bytes || b->bytes != expected_bytes)
            throw std::invalid_argument("DOP padded bytes disagree with allocations");
        if (a->identity.allocation_generation == 0
                || b->identity.allocation_generation == 0
                || a->identity.allocation_generation == b->identity.allocation_generation
                || a->identity.tenant != b->identity.tenant
                || a->identity.device != b->identity.device)
            throw std::invalid_argument("foreign or duplicate GPU allocation identities");
        if (a->sequence == std::numeric_limits<std::uint64_t>::max()
                || b->sequence == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("device ticket sequence exhausted");
        // Allocate two fresh, per-submission seals and copy identity strings
        // before changing either allocation. No throwing operations remain.
        auto seal_a = std::make_shared<const std::uint8_t>(0);
        auto seal_b = std::make_shared<const std::uint8_t>(0);
        PairTickets tickets{
            a->identity, Ticket(a->identity.allocation_generation, a->sequence + 1, seal_a),
            b->identity, Ticket(b->identity.allocation_generation, b->sequence + 1, seal_b)};
        a->active_seal = std::move(seal_a);
        b->active_seal = std::move(seal_b);
        ++a->sequence;
        ++b->sequence;
        a->backend_quiescent = false;
        b->backend_quiescent = false;
        a->status = Status::InFlight;
        b->status = Status::InFlight;
        return tickets;
    }

    Ticket begin_command(const Identity& caller) {
        std::lock_guard<std::mutex> owner_guard(owner_lock_);
        active();
        std::lock_guard<std::mutex> guard(state_->lock);
        if (!(state_->identity == caller)) throw std::logic_error("foreign device identity");
        if (state_->status != Status::Idle) throw std::logic_error("buffer not idle");
        if (state_->sequence == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("device ticket sequence exhausted");
        auto seal = std::make_shared<const std::uint8_t>(0);
        Ticket ticket(state_->identity.allocation_generation, state_->sequence + 1, seal);
        state_->active_seal = std::move(seal);
        ++state_->sequence;
        state_->backend_quiescent = false;
        state_->status = Status::InFlight;
        return ticket;
    }

    void complete(Ticket ticket, bool succeeded) {
        std::lock_guard<std::mutex> owner_guard(owner_lock_);
        active();
        std::lock_guard<std::mutex> guard(state_->lock);
        validate_inflight(ticket);
        state_->status = succeeded ? Status::Idle : Status::Poisoned;
        state_->backend_quiescent = true;
        state_->active_seal.reset();
    }
    void cancel_pending(Ticket ticket) {
        std::lock_guard<std::mutex> owner_guard(owner_lock_);
        active();
        std::lock_guard<std::mutex> guard(state_->lock);
        validate_inflight(ticket);
        state_->status = Status::Poisoned;
    }
    void acknowledge_cancelled_device_event(Ticket ticket, const Identity& caller) {
        std::lock_guard<std::mutex> owner_guard(owner_lock_);
        active();
        std::lock_guard<std::mutex> guard(state_->lock);
        if (!(state_->identity == caller) || state_->status != Status::Poisoned
            || state_->backend_quiescent
            || ticket.generation != state_->identity.allocation_generation
            || ticket.sequence != state_->sequence
            || !state_->active_seal || ticket.seal_ != state_->active_seal)
            throw std::logic_error("stale cancelled device fence acknowledgement");
        state_->backend_quiescent = true;
        state_->active_seal.reset();
    }
    void retire_after_backend_release(const Identity& caller) {
        std::lock_guard<std::mutex> owner_guard(owner_lock_);
        active();
        std::lock_guard<std::mutex> guard(state_->lock);
        if (!(state_->identity == caller)) throw std::logic_error("foreign device identity");
        if (state_->status == Status::InFlight || !state_->backend_quiescent)
            throw std::logic_error("device command not yet quiescent");
        state_->status = Status::Closed;
    }
    void close_after_retirement() const {
        std::lock_guard<std::mutex> owner_guard(owner_lock_);
        if (!state_) return;
        std::lock_guard<std::mutex> guard(state_->lock);
        if (state_->status != Status::Closed)
            throw std::logic_error("backend must release device buffer first");
    }
    Status status() const {
        std::lock_guard<std::mutex> owner_guard(owner_lock_);
        active();
        std::lock_guard<std::mutex> guard(state_->lock);
        return state_->status;
    }
    // A per-allocation metadata snapshot, not provider authorization.
    struct Snapshot { Identity identity; std::uint64_t bytes; Status status; };
    Snapshot snapshot() const {
        std::lock_guard<std::mutex> owner_guard(owner_lock_);
        active();
        std::lock_guard<std::mutex> guard(state_->lock);
        return Snapshot{state_->identity, state_->bytes, state_->status};
    }

private:
    explicit BufferFence(std::shared_ptr<State> state) : state_(std::move(state)) {}
    void active() const {
        // Every caller holds owner_lock_, including const snapshot/status.
        if (!state_) throw std::logic_error("device ownership moved");
        std::lock_guard<std::mutex> guard(state_->lock);
        if (state_->status == Status::Closed) throw std::logic_error("device buffer closed");
    }
    void validate_inflight(Ticket ticket) const {
        if (state_->status != Status::InFlight ||
            ticket.generation != state_->identity.allocation_generation ||
            ticket.sequence != state_->sequence ||
            !state_->active_seal || ticket.seal_ != state_->active_seal)
            throw std::logic_error("stale or mismatched device event");
    }
    mutable std::mutex owner_lock_;
    std::shared_ptr<State> state_;
};

} // namespace oreslang::gpu
