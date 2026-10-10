#pragma once

#include <atomic>
#include <cstdint>

namespace oreslang::runtime {

// An owned operation identity. The caller, not this gate, must retain the
// allocation/actor frame until completion callbacks can no longer access it.
struct AsyncIoTicket {
    std::uint64_t actor_id;
    std::uint64_t generation;
    std::uint64_t operation_id;
};

enum class AsyncIoTerminal : std::uint8_t {
    pending, success, failure, cancelled, timed_out, closed
};

// Allocation-free, wait-free terminal arbitration for an already-admitted I/O
// operation. Does not submit I/O, create async frames, own buffers or wake actors.
class AsyncIoCompletionGate final {
public:
    explicit AsyncIoCompletionGate(AsyncIoTicket ticket) noexcept : ticket_(ticket) {}

    bool finish(AsyncIoTicket ticket, AsyncIoTerminal outcome) noexcept {
        if (!valid(ticket) || outcome == AsyncIoTerminal::pending) return false;
        AsyncIoTerminal expected = AsyncIoTerminal::pending;
        return terminal_.compare_exchange_strong(expected, outcome,
                std::memory_order_acq_rel, std::memory_order_acquire);
    }

    AsyncIoTerminal terminal() const noexcept {
        return terminal_.load(std::memory_order_acquire);
    }

    const AsyncIoTicket& ticket() const noexcept { return ticket_; }

private:
    bool valid(AsyncIoTicket ticket) const noexcept {
        return ticket.actor_id == ticket_.actor_id
            && ticket.generation == ticket_.generation
            && ticket.operation_id == ticket_.operation_id
            && ticket.actor_id != 0 && ticket.generation != 0
            && ticket.operation_id != 0;
    }

    const AsyncIoTicket ticket_;
    std::atomic<AsyncIoTerminal> terminal_{AsyncIoTerminal::pending};
};

} // namespace oreslang::runtime
