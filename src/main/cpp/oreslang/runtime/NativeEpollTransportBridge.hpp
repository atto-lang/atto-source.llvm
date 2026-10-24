#pragma once
#if !defined(__linux__)
#error "NativeEpollTransportBridge requires Linux"
#endif

#include "oreslang/runtime/NativeEpollReactor.hpp"
#include "oreslang/runtime/NativeTransportCallbackGate.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <utility>

namespace oreslang::runtime {

/**
 * Trusted host-only, bounded epoll-to-actor callback ownership bridge.
 *
 * A transport permit is acquired BEFORE a one-shot epoll watch is published,
 * stored in one preallocated slot until its terminal record is consumed, then
 * MOVED into the completion envelope. The envelope retains it until the host
 * finishes all actor/request memory access, including cancellation cleanup.
 * Each watch produces exactly one ready/cancelled/stopped terminal record.
 * No callbacks, guest functions or actor code execute on the epoll thread.
 *
 * A native reactor can only be accessed through this wrapper. No allocation
 * occurs on its watch/cancel/pop paths; the only thread creation is start().
 * CompletionLease must not outlive the referenced callback gate's storage.
 */
class NativeEpollTransportBridge final {
public:
    using Reactor = NativeEpollReactor;
    using Gate = NativeTransportCallbackGate;
    using Ticket = Reactor::Ticket;
    struct CompletionLease final {
        Reactor::Completion event;
        Gate::Permit permit;
        CompletionLease(const CompletionLease&) = delete;
        CompletionLease& operator=(const CompletionLease&) = delete;
        CompletionLease(CompletionLease&&) noexcept = default;
        CompletionLease& operator=(CompletionLease&&) noexcept = default;
        CompletionLease(Reactor::Completion e, Gate::Permit p) noexcept
            : event(e), permit(std::move(p)) {}
    };

    explicit NativeEpollTransportBridge(Gate& gate) noexcept : gate_(gate) {}
    NativeEpollTransportBridge(const NativeEpollTransportBridge&) = delete;
    NativeEpollTransportBridge& operator=(const NativeEpollTransportBridge&) = delete;

    ~NativeEpollTransportBridge() noexcept {
        stop(); // joined reactor; all outstanding results must be consumed
        for (const auto& item : slots_) {
            if (item.token != 0 || item.permit) std::abort();
        }
        // try_pop() moves the permit out of the preallocated slot: an empty
        // slot table does not establish that the downstream actor carrier is
        // done touching arena memory. Prevent bridge destruction until ALL
        // registered callback permits have been released and shutdown won.
        if (!gate_.try_drain()) std::abort();
    }

    // Only trusted host bootstrap/teardown may block for thread creation.
    bool start() {
        std::lock_guard<std::mutex> guard(mu_);
        return !stopped_ && gate_.accepting() && reactor_.start();
    }

    // Nonparking actor/host admission; reactor may complete immediately.
    // Our lock prevents try_pop from racing the slot-permit installation.
    [[nodiscard]] std::optional<Ticket> try_watch(
            int fd, std::uint32_t events) noexcept {
        auto permit = gate_.try_admit();
        if (!permit) return std::nullopt;
        std::unique_lock<std::mutex> lock(mu_, std::try_to_lock);
        if (!lock.owns_lock() || stopped_) return std::nullopt;
        auto ticket = reactor_.try_watch(fd, events);
        if (!ticket) return std::nullopt;
        const auto index = ticket_index(*ticket);
        if (index >= slots_.size()) std::abort();
        auto& item = slots_[index];
        if (item.token != 0 || item.permit) std::abort();
        item.token = ticket->value;
        item.permit = std::move(permit);
        return ticket;
    }

    // Cancellation only publishes a terminal completion. It must NEVER
    // release the callback permit before the completion consumer finishes.
    bool try_cancel(Ticket ticket) noexcept {
        std::unique_lock<std::mutex> lock(mu_, std::try_to_lock);
        return lock.owns_lock() && !stopped_ && reactor_.try_cancel(ticket);
    }

    // The returned move-only envelope owns the permit until final cleanup.
    // If moved into a scheduler mailbox, the mailbox must retain it until
    // the last consumer has stopped accessing the associated actor arena.
    [[nodiscard]] std::optional<CompletionLease> try_pop() noexcept {
        std::unique_lock<std::mutex> lock(mu_, std::try_to_lock);
        if (!lock.owns_lock()) return std::nullopt;
        auto event = reactor_.try_pop();
        if (!event) return std::nullopt;
        const auto index = ticket_index(event->ticket);
        if (index >= slots_.size()) std::abort();
        auto& item = slots_[index];
        if (item.token != event->ticket.value || !item.permit) std::abort();
        CompletionLease result{*event, std::move(item.permit)};
        item.token = 0;
        return result;
    }

    // Freeze new callback admission first, then stop/join the epoll worker.
    // The worker publishes stopped completions for every remaining watch.
    // The host must still pop and retire each terminal envelope to drain.
    void stop() noexcept {
        gate_.stop_admission();
        std::lock_guard<std::mutex> lock(mu_);
        if (stopped_) return;
        stopped_ = true;
        reactor_.stop();
    }

    int completion_fd() const noexcept { return reactor_.completion_fd(); }
    void drain_completion_signal() noexcept {
        reactor_.drain_completion_signal();
    }

private:
    struct Slot final {
        std::uint64_t token = 0;
        Gate::Permit permit;
    };
    static std::size_t ticket_index(Ticket ticket) noexcept {
        const auto low = static_cast<std::uint32_t>(ticket.value);
        return low == 0 ? Reactor::capacity
                        : static_cast<std::size_t>(low - 1);
    }

    Gate& gate_;
    Reactor reactor_;
    std::mutex mu_;
    std::array<Slot, Reactor::capacity> slots_{};
    bool stopped_ = false;
};

} // namespace oreslang::runtime
