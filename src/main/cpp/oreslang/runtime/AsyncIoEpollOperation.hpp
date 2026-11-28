#pragma once

#if !defined(__linux__)
#error "AsyncIoEpollOperation is Linux-only"
#endif

#include "oreslang/runtime/AsyncIoOperationLifecycle.hpp"
#include "oreslang/runtime/NonblockingTcpLinux.hpp"

#include <cstdint>
#include <mutex>
#include <stdexcept>

namespace oreslang::runtime {

// Binds one lifecycle authority to one one-shot epoll registration.
//
// The reactor thread may block inside EpollReadiness::wait(). Actor/carrier
// threads only arm, cancel, or consume completion state; they never call wait().
// Serialize the operation's kernel registration transitions, not guest execution.
// The owning reactor must retain the fd and this object until all callers exit.
class AsyncIoEpollOperation final {
public:
    AsyncIoEpollOperation(
            EpollReadiness& readiness,
            AsyncIoOperationLifecycle& lifecycle,
            AsyncIoTicket ticket,
            int fd,
            std::uint32_t events,
            std::uint64_t registration_token)
        : readiness_(readiness),
          lifecycle_(lifecycle),
          ticket_(ticket),
          fd_(fd),
          events_(events),
          registration_token_(registration_token) {
        EpollReadiness::validate_registration(fd_, events_, registration_token_);
        const auto& expected = lifecycle_.ticket();
        if (ticket_.actor_id == 0 || ticket_.generation == 0 || ticket_.operation_id == 0
                || ticket_.actor_id != expected.actor_id
                || ticket_.generation != expected.generation
                || ticket_.operation_id != expected.operation_id)
            throw std::invalid_argument("invalid async I/O ticket");
    }

    // The owner is responsible for keeping this object and its lifecycle alive
    // when disarm cannot confirm removal. Destruction is not a retirement ACK.
    ~AsyncIoEpollOperation() noexcept { disarm(); }

    AsyncIoEpollOperation(const AsyncIoEpollOperation&) = delete;
    AsyncIoEpollOperation& operator=(const AsyncIoEpollOperation&) = delete;

    void arm() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (armed_) throw std::logic_error("async I/O operation already armed");
        if (!lifecycle_.mark_registered(ticket_))
            throw std::logic_error("async I/O lifecycle rejected registration");
        try {
            readiness_.add(fd_, events_, registration_token_);
            armed_ = true;
        } catch (...) {
            (void) lifecycle_.mark_unregistered(ticket_);
            (void) lifecycle_.finish(ticket_, AsyncIoTerminal::failure);
            throw;
        }
    }

    // Returns true only for this operation's one-shot readiness notification
    // and only while terminal arbitration is still pending.
    bool claim_ready(const EpollReadiness::Ready& ready) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!armed_ || ready.token != registration_token_) return false;
        const std::uint32_t terminal_events =
            static_cast<std::uint32_t>(EPOLLERR | EPOLLHUP | EPOLLRDHUP);
        if ((ready.events & (events_ | terminal_events)) == 0) return false;
        return lifecycle_.begin_callback(ticket_);
    }

    // Called after a readiness callback consumed no bytes because the fd raced
    // back to EAGAIN/EWOULDBLOCK. The callback must retire before rearming.
    bool rearm_after_would_block() {
        std::lock_guard<std::mutex> lock(mutex_);
        // The physical callback still owns its credit even if cancellation
        // already removed the registration. Release it before checking rearm.
        if (!lifecycle_.end_callback(ticket_)) return false;
        if (!armed_ || lifecycle_.terminal() != AsyncIoTerminal::pending)
            return false;
        try {
            readiness_.rearm(fd_, events_, registration_token_);
            return true;
        } catch (...) {
            (void) lifecycle_.finish(ticket_, AsyncIoTerminal::failure);
            disarm_locked();
            throw;
        }
    }

    // Terminal callback completion. If cancellation/timeout already won, this
    // returns false but still retires callback ownership.
    bool finish_callback(AsyncIoTerminal outcome) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!valid_async_io_terminal(outcome) || !lifecycle_.callback_inflight())
            return false;
        const bool won = lifecycle_.finish(ticket_, outcome);
        const bool ended = lifecycle_.end_callback(ticket_);
        return won && ended;
    }

    // Cancellation settles the Future first, removes kernel readiness, then
    // leaves physical callback retirement (if any) to the callback owner.
    bool cancel() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool won = lifecycle_.cancel(ticket_);
        disarm_locked();
        return won;
    }

    bool timeout() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool won = lifecycle_.timeout(ticket_);
        disarm_locked();
        return won;
    }

    void disarm() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        disarm_locked();
    }

    bool retire() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return lifecycle_.retire(ticket_);
    }

    bool armed() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return armed_;
    }
    std::uint64_t registration_token() const noexcept { return registration_token_; }

private:
    void disarm_locked() noexcept {
        if (!armed_) return;
        // Do not refund physical registration credit on ambiguous kernel
        // failure. The owner must quarantine the operation and retain its
        // frame until removal is confirmed under the same fd identity.
        if (!readiness_.remove_confirmed(fd_)) {
            (void) lifecycle_.finish(ticket_, AsyncIoTerminal::failure);
            return;
        }
        (void) lifecycle_.mark_unregistered(ticket_);
        armed_ = false;
    }

    mutable std::mutex mutex_;
    EpollReadiness& readiness_;
    AsyncIoOperationLifecycle& lifecycle_;
    AsyncIoTicket ticket_;
    int fd_;
    std::uint32_t events_;
    std::uint64_t registration_token_;
    bool armed_ = false;
};

} // namespace oreslang::runtime
