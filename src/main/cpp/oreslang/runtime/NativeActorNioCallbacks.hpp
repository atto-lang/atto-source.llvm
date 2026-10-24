#pragma once
#if !defined(__linux__)
#error "NativeActorNioCallbacks requires Linux"
#endif
#include "oreslang/runtime/NativeEpollReactorABI.h"
#include "oreslang/runtime/NativeEpollReactorManager.hpp"
#include "oreslang/runtime/NativeTransportCallbackGate.hpp"
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <utility>

namespace oreslang::runtime {

// Trusted HOST-ONLY actor NIO substrate. Legacy pop MUST NOT be used on a
// manager in actor-owned mode: it cannot carry callback ownership receipts.
// Each watched FD consumes a slot until its terminal completion is explicitly
// acknowledged. No per-watch heap allocations or blocking locks on hot paths.
class NativeActorNioCallbacks final {
public:
    using Manager = NativeEpollReactorManager;
    using Gate = NativeTransportCallbackGate;
    static constexpr std::size_t capacity =
        Manager::maximum_reactors * NativeEpollReactor::capacity;
    struct Completion {
        OresNativeNioCompletion event{};
        Gate::Binding owner{};
        std::uint64_t lease_id = 0;
    };

    NativeActorNioCallbacks() = default;
    NativeActorNioCallbacks(const NativeActorNioCallbacks&) = delete;
    NativeActorNioCallbacks& operator=(const NativeActorNioCallbacks&) = delete;
    ~NativeActorNioCallbacks() noexcept { fail_stop_if_outstanding(); }

    // The producer retains FD ownership and the exact gate's lifetime. Watch
    // admission and completion consumption are serialized through this short,
    // nonparking try-lock. A completion never outruns permit installation.
    bool try_watch(Manager& manager, Gate& gate, int fd,
                   std::uint32_t events, OresNativeNioWatch* output) noexcept {
        if (!output) return false;
        std::unique_lock<std::mutex> lock(mu_, std::try_to_lock);
        if (!lock.owns_lock()) return false;
        Entry* free = nullptr;
        for (auto& item : entries_) {
            // The reactor releases its FD claim when try_pop() consumes the
            // kernel completion, but the actor's physical callback loan ends
            // only at acknowledge(). Do not let another actor or generation
            // register the same FD while an older completion is outstanding.
            if (item.occupied && item.fd == fd) return false;
            if (!item.occupied && !free) free = &item;
        }
        if (!free) return false;
        const auto id = reserve_lease_id();
        if (!id) return false;
        auto permit = gate.try_admit();
        if (!permit) return false;
        auto watch = manager.try_watch(fd, events);
        if (!watch) return false;
        free->occupied = true;
        free->delivered = false;
        free->fd = fd;
        free->terminal = {};
        free->watch = OresNativeNioWatch{watch->ticket.value, watch->reactor};
        free->owner = gate.binding();
        free->lease_id = *id;
        free->permit = std::move(permit);
        *output = free->watch;
        return true;
    }

    bool try_cancel(Manager& manager, OresNativeNioWatch watch) noexcept {
        std::unique_lock<std::mutex> lock(mu_, std::try_to_lock);
        if (!lock.owns_lock()) return false;
        for (const auto& item : entries_) {
            if (item.occupied && !item.delivered && same_watch(item.watch, watch)) {
                return manager.try_cancel(Manager::Watch{
                    watch.reactor, NativeEpollReactor::Ticket{watch.ticket}});
            }
        }
        return false;
    }

    bool try_pop(Manager& manager, Completion* output) noexcept {
        if (!output) return false;
        std::unique_lock<std::mutex> lock(mu_, std::try_to_lock);
        if (!lock.owns_lock()) return false;
        auto result = manager.try_pop();
        if (!result) return false;
        const OresNativeNioWatch watch{result->event.ticket.value, result->reactor};
        for (auto& item : entries_) {
            if (!item.occupied || item.delivered || !same_watch(item.watch, watch))
                continue;
            OresNativeNioOutcome outcome;
            switch (result->event.outcome) {
                case NativeEpollReactor::Outcome::ready:
                    outcome = ORES_NATIVE_NIO_READY; break;
                case NativeEpollReactor::Outcome::cancelled:
                    outcome = ORES_NATIVE_NIO_CANCELLED; break;
                case NativeEpollReactor::Outcome::stopped:
                    outcome = ORES_NATIVE_NIO_STOPPED; break;
                default: std::abort();
            }
            // Validate the reactor completion against its original FD
            // before transferring an actor-visible terminal record.
            if (result->event.fd != item.fd) std::abort();
            item.terminal = OresNativeNioCompletion{
                watch, result->event.fd, result->event.events, outcome};
            item.delivered = true;
            *output = Completion{item.terminal, item.owner, item.lease_id};
            return true;
        }
        // Owned completions must never silently lose their accounting permit.
        // In particular, the legacy consumer cannot consume this same queue.
        std::abort();
    }

    bool acknowledge(const Completion& completion) noexcept {
        if (!completion.lease_id) return false;
        std::unique_lock<std::mutex> lock(mu_, std::try_to_lock);
        if (!lock.owns_lock()) return false;
        for (auto& item : entries_) {
            if (!item.occupied || !item.delivered ||
                    item.lease_id != completion.lease_id ||
                    !same_watch(item.watch, completion.event.watch) ||
                    !same_owner(item.owner, completion.owner) ||
                    item.terminal.fd != completion.event.fd ||
                    item.terminal.events != completion.event.events ||
                    item.terminal.outcome != completion.event.outcome) continue;
            // The caller attests this is the final physical actor-memory
            // access. Reject altered completion metadata before releasing
            // the callback permit and allowing the FD to be reused.
            item.permit.release();
            item.occupied = false;
            item.delivered = false;
            item.lease_id = 0;
            item.fd = -1;
            item.terminal = {};
            return true;
        }
        return false;
    }

    void fail_stop_if_outstanding() const noexcept {
        std::lock_guard<std::mutex> lock(mu_); // trusted host destructor only
        for (const auto& item : entries_)
            if (item.occupied || item.permit) std::abort();
    }

private:
    struct Entry {
        OresNativeNioWatch watch{};
        OresNativeNioCompletion terminal{};
        Gate::Binding owner{};
        Gate::Permit permit{};
        std::uint64_t lease_id = 0;
        int fd = -1;
        bool occupied = false;
        bool delivered = false;
    };
    static bool same_watch(OresNativeNioWatch left, OresNativeNioWatch right) noexcept {
        return left.ticket == right.ticket && left.reactor == right.reactor;
    }
    static bool same_owner(Gate::Binding left, Gate::Binding right) noexcept {
        return left.domain == right.domain && left.slot == right.slot &&
            left.generation == right.generation && left.slab == right.slab;
    }
    static std::optional<std::uint64_t> reserve_lease_id() noexcept {
        // Process-global monotonic identity: a stale claim can never match
        // a new VM backend that happened to reuse its old address.
        static std::atomic<std::uint64_t> next{1};
        auto observed = next.load(std::memory_order_relaxed);
        for (;;) {
            if (observed == UINT64_MAX || observed == 0) return std::nullopt;
            if (next.compare_exchange_weak(observed, observed + 1,
                    std::memory_order_relaxed, std::memory_order_relaxed))
                return observed;
        }
    }
    mutable std::mutex mu_;
    std::array<Entry, capacity> entries_{};
};

} // namespace oreslang::runtime
