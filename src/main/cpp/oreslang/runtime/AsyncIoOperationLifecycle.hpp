#pragma once

#include "oreslang/runtime/AsyncIoCompletionGate.hpp"

#include <atomic>
#include <cstdint>

namespace oreslang::runtime {

// Host-only lifecycle authority for one admitted asynchronous I/O operation.
//
// Logical completion and physical retirement are deliberately separate:
// a Future may settle because cancellation/timeout won while a reactor callback
// is still in flight. Actor teardown/get_done must key off ready_to_reclaim(),
// never terminal() alone.
class AsyncIoOperationLifecycle final {
public:
    explicit AsyncIoOperationLifecycle(AsyncIoTicket ticket) noexcept
        : gate_(ticket) {}

    const AsyncIoTicket& ticket() const noexcept { return gate_.ticket(); }

    bool mark_registered(AsyncIoTicket ticket) noexcept {
        if (!valid(ticket) || retired_.load(std::memory_order_acquire)
                || gate_.terminal() != AsyncIoTerminal::pending)
            return false;
        bool expected = false;
        return registered_.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel, std::memory_order_acquire);
    }

    // Reactor callback ownership is single-flight. Cancellation may race after
    // this succeeds; in that case the callback must observe that it lost terminal
    // arbitration, perform no guest continuation resume, and still call end_callback.
    bool begin_callback(AsyncIoTicket ticket) noexcept {
        if (!valid(ticket)
                || !registered_.load(std::memory_order_acquire)
                || retired_.load(std::memory_order_acquire)
                || gate_.terminal() != AsyncIoTerminal::pending)
            return false;
        bool expected = false;
        return callback_inflight_.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel, std::memory_order_acquire);
    }

    bool finish(AsyncIoTicket ticket, AsyncIoTerminal outcome) noexcept {
        return gate_.finish(ticket, outcome);
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
        if (!valid(ticket)) return false;
        bool expected = true;
        return callback_inflight_.compare_exchange_strong(
                expected, false, std::memory_order_acq_rel, std::memory_order_acquire);
    }

    // Must be called only after epoll/kqueue/IOCP ownership is physically removed.
    bool mark_unregistered(AsyncIoTicket ticket) noexcept {
        if (!valid(ticket)) return false;
        bool expected = true;
        return registered_.compare_exchange_strong(
                expected, false, std::memory_order_acq_rel, std::memory_order_acquire);
    }

    // Reclamation is legal only after a terminal winner exists and every native
    // registration/callback has physically retired.
    bool retire(AsyncIoTicket ticket) noexcept {
        if (!valid(ticket)
                || gate_.terminal() == AsyncIoTerminal::pending
                || registered_.load(std::memory_order_acquire)
                || callback_inflight_.load(std::memory_order_acquire))
            return false;
        bool expected = false;
        return retired_.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel, std::memory_order_acquire);
    }

    AsyncIoTerminal terminal() const noexcept { return gate_.terminal(); }
    bool registered() const noexcept { return registered_.load(std::memory_order_acquire); }
    bool callback_inflight() const noexcept {
        return callback_inflight_.load(std::memory_order_acquire);
    }
    bool physically_retired() const noexcept { return retired_.load(std::memory_order_acquire); }

    // Scheduler/actor lifecycle barrier: only this state may release the frame
    // containing buffers, continuation identity and actor-owned operation state.
    bool ready_to_reclaim() const noexcept {
        return physically_retired()
            && gate_.terminal() != AsyncIoTerminal::pending
            && !registered()
            && !callback_inflight();
    }

private:
    bool valid(AsyncIoTicket ticket) const noexcept {
        const auto& expected = gate_.ticket();
        return ticket.actor_id != 0 && ticket.generation != 0 && ticket.operation_id != 0
            && ticket.actor_id == expected.actor_id
            && ticket.generation == expected.generation
            && ticket.operation_id == expected.operation_id;
    }

    AsyncIoCompletionGate gate_;
    std::atomic<bool> registered_{false};
    std::atomic<bool> callback_inflight_{false};
    std::atomic<bool> retired_{false};
};

} // namespace oreslang::runtime
