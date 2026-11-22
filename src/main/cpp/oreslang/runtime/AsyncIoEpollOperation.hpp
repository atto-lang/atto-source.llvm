#pragma once

#if !defined(__linux__)
#error "AsyncIoEpollOperation is Linux-only"
#endif

#include "oreslang/runtime/AsyncIoOperationLifecycle.hpp"
#include "oreslang/runtime/NonblockingTcpLinux.hpp"

#include <cstdint>
#include <stdexcept>

namespace oreslang::runtime {

// Binds one lifecycle authority to one one-shot epoll registration.
//
// The reactor thread may block inside EpollReadiness::wait(). Actor/carrier
// threads only arm, cancel, or consume completion state; they never call wait().
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
        if (ticket_.actor_id == 0 || ticket_.generation == 0 || ticket_.operation_id == 0)
            throw std::invalid_argument("invalid async I/O ticket");
    }

    ~AsyncIoEpollOperation() noexcept { disarm(); }

    AsyncIoEpollOperation(const AsyncIoEpollOperation&) = delete;
    AsyncIoEpollOperation& operator=(const AsyncIoEpollOperation&) = delete;

    void arm() {
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
        if (!armed_ || ready.token != registration_token_) return false;
        const std::uint32_t terminal_events =
            static_cast<std::uint32_t>(EPOLLERR | EPOLLHUP | EPOLLRDHUP);
        if ((ready.events & (events_ | terminal_events)) == 0) return false;
        return lifecycle_.begin_callback(ticket_);
    }

    // Called after a readiness callback consumed no bytes because the fd raced
    // back to EAGAIN/EWOULDBLOCK. The callback must retire before rearming.
    bool rearm_after_would_block() {
        if (!armed_ || lifecycle_.terminal() != AsyncIoTerminal::pending)
            return false;
        if (!lifecycle_.end_callback(ticket_)) return false;
        try {
            readiness_.rearm(fd_, events_, registration_token_);
            return true;
        } catch (...) {
            (void) lifecycle_.finish(ticket_, AsyncIoTerminal::failure);
            disarm();
            throw;
        }
    }

    // Terminal callback completion. If cancellation/timeout already won, this
    // returns false but still retires callback ownership.
    bool finish_callback(AsyncIoTerminal outcome) noexcept {
        if (outcome == AsyncIoTerminal::pending) return false;
        const bool won = lifecycle_.finish(ticket_, outcome);
        const bool ended = lifecycle_.end_callback(ticket_);
        return won && ended;
    }

    // Cancellation settles the Future first, removes kernel readiness, then
    // leaves physical callback retirement (if any) to the callback owner.
    bool cancel() noexcept {
        const bool won = lifecycle_.cancel(ticket_);
        disarm();
        return won;
    }

    bool timeout() noexcept {
        const bool won = lifecycle_.timeout(ticket_);
        disarm();
        return won;
    }

    void disarm() noexcept {
        if (!armed_) return;
        readiness_.remove(fd_);
        (void) lifecycle_.mark_unregistered(ticket_);
        armed_ = false;
    }

    bool retire() noexcept {
        return lifecycle_.retire(ticket_);
    }

    bool armed() const noexcept { return armed_; }
    std::uint64_t registration_token() const noexcept { return registration_token_; }

private:
    EpollReadiness& readiness_;
    AsyncIoOperationLifecycle& lifecycle_;
    AsyncIoTicket ticket_;
    int fd_;
    std::uint32_t events_;
    std::uint64_t registration_token_;
    bool armed_ = false;
};

} // namespace oreslang::runtime
