#pragma once

#include "oreslang/runtime/AsyncIoCompletionGate.hpp"

#include <atomic>
#include <cstdint>

namespace oreslang::runtime {

// Host-only lifecycle authority for one admitted asynchronous I/O operation.
// Logical completion and physical retirement remain separate, but all flags
// and the terminal outcome linearize on one atomic word. In particular a
// registration or callback cannot slip between retirement's checks and CAS.
// The caller must still keep the object alive until all API users quiesce.
class AsyncIoOperationLifecycle final {
public:
    explicit AsyncIoOperationLifecycle(AsyncIoTicket ticket) noexcept
        : ticket_(ticket) {}

    const AsyncIoTicket& ticket() const noexcept { return ticket_; }

    bool mark_registered(AsyncIoTicket ticket) noexcept {
        return valid(ticket) && transition(terminal_mask | registered_bit | retired_bit,
                                           0, registered_bit, 0);
    }

    // Cancellation after a successful claim still requires end_callback().
    bool begin_callback(AsyncIoTicket ticket) noexcept {
        return valid(ticket) && transition(terminal_mask | registered_bit |
                                           callback_bit | retired_bit,
                                           registered_bit, callback_bit, 0);
    }

    bool finish(AsyncIoTicket ticket, AsyncIoTerminal outcome) noexcept {
        return valid(ticket) && valid_async_io_terminal(outcome) &&
            transition(terminal_mask | retired_bit, 0,
                       static_cast<std::uint32_t>(outcome), 0);
    }

    bool cancel(AsyncIoTicket ticket) noexcept {
        return finish(ticket, AsyncIoTerminal::cancelled);
    }

    bool timeout(AsyncIoTicket ticket) noexcept {
        return finish(ticket, AsyncIoTerminal::timed_out);
    }

    bool close(AsyncIoTicket ticket) noexcept {
        return finish(ticket, AsyncIoTerminal::closed);
    }

    bool end_callback(AsyncIoTicket ticket) noexcept {
        return valid(ticket) && transition(callback_bit | retired_bit,
                                           callback_bit, 0, callback_bit);
    }

    // Must be called only after epoll/kqueue/IOCP ownership is physically removed.
    bool mark_unregistered(AsyncIoTicket ticket) noexcept {
        return valid(ticket) && transition(registered_bit | retired_bit,
                                           registered_bit, 0, registered_bit);
    }

    bool retire(AsyncIoTicket ticket) noexcept {
        if (!valid(ticket)) return false;
        auto state = state_.load(std::memory_order_acquire);
        do {
            if (!(state & terminal_mask) ||
                (state & (registered_bit | callback_bit | retired_bit))) return false;
        } while (!state_.compare_exchange_weak(state, state | retired_bit,
                    std::memory_order_acq_rel, std::memory_order_acquire));
        return true;
    }

    AsyncIoTerminal terminal() const noexcept {
        return static_cast<AsyncIoTerminal>(state_.load(std::memory_order_acquire) & terminal_mask);
    }
    bool registered() const noexcept { return (state_.load(std::memory_order_acquire) & registered_bit) != 0; }
    bool callback_inflight() const noexcept {
        return (state_.load(std::memory_order_acquire) & callback_bit) != 0;
    }
    bool physically_retired() const noexcept { return (state_.load(std::memory_order_acquire) & retired_bit) != 0; }

    // Once retired, no subsequent transition can resurrect ownership.
    bool ready_to_reclaim() const noexcept { return physically_retired(); }

private:
    static constexpr std::uint32_t terminal_mask = 0xffu;
    static constexpr std::uint32_t registered_bit = 1u << 8;
    static constexpr std::uint32_t callback_bit = 1u << 9;
    static constexpr std::uint32_t retired_bit = 1u << 10;

    bool transition(std::uint32_t mask, std::uint32_t required,
                    std::uint32_t set, std::uint32_t clear) noexcept {
        auto state = state_.load(std::memory_order_acquire);
        do {
            if ((state & mask) != required) return false;
        } while (!state_.compare_exchange_weak(state, (state | set) & ~clear,
                    std::memory_order_acq_rel, std::memory_order_acquire));
        return true;
    }

    bool valid(AsyncIoTicket ticket) const noexcept {
        return ticket.actor_id != 0 && ticket.generation != 0 && ticket.operation_id != 0
            && ticket.actor_id == ticket_.actor_id
            && ticket.generation == ticket_.generation
            && ticket.operation_id == ticket_.operation_id;
    }

    const AsyncIoTicket ticket_;
    std::atomic<std::uint32_t> state_{0};
};

} // namespace oreslang::runtime
