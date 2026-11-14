#pragma once

// Actor-confined bridge over allocator-minted native Box tickets.
// Experimental C++20 runtime helper; NOT a guest-accessible capability,
// not actor allocation relocation and not a replacement for the scheduler's
// quiescence fence or compiler's borrow and suspension verification.

#include "oreslang/runtime/ActorHeapIdentity.hpp"
#include "oreslang/runtime/NativeBoxArenaV1.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>

namespace oreslang::runtime::native_ownership_v1 {

template <typename T, std::size_t Slots>
class ActorBoxRegion final {
    static_assert(Slots > 0, "actor box region requires slots");
    ActorHeapIdentity identity_;
    FixedBoxArena<T, Slots> arena_{};
    std::array<UniqueBox<T>, Slots> live_{};
    // Bind once under an admission-only mutex. No mutex on the actor hot path.
    std::mutex bind_lock_;
    std::thread::id carrier_{};
    std::atomic<bool> bound_{false};
    // May be read by a competing bind_carrier; unlike the carrier-local
    // retired/dropping flags, shutdown admission is synchronized.
    std::atomic<bool> shutting_down_{false};
    bool retired_ = false;
    bool executing_callback_ = false;
    bool dropping_ = false;

    bool on_actor_carrier(ActorHeapIdentity requested) const noexcept {
        return !shutting_down_.load(std::memory_order_acquire) &&
               bound_.load(std::memory_order_acquire) &&
               std::this_thread::get_id() == carrier_ &&
               requested == identity_;
    }

public:
    explicit ActorBoxRegion(ActorHeapIdentity trusted_actor) noexcept
        : identity_(trusted_actor) {}
    ActorBoxRegion(const ActorBoxRegion&) = delete;
    ActorBoxRegion& operator=(const ActorBoxRegion&) = delete;
    ActorBoxRegion(ActorBoxRegion&&) = delete;
    ActorBoxRegion& operator=(ActorBoxRegion&&) = delete;

    ~ActorBoxRegion() noexcept {
        // The scheduler must already have fenced callbacks and foreign
        // carriers. Synchronize the admission lock with bind_carrier, then
        // revoke all new calls BEFORE executing any user-defined destructor.
        // A T::~T() is permitted to call back into the region, but must not
        // observe an allocatable or bindable region during destruction.
        {
            std::lock_guard<std::mutex> lock(bind_lock_);
            if (bound_.load(std::memory_order_acquire) &&
                    std::this_thread::get_id() != carrier_)
                std::terminate();
            if (executing_callback_ || dropping_) std::terminate();
            shutting_down_.store(true, std::memory_order_release);
        }
        retired_ = true;
        dropping_ = true;
        for (auto& box : live_) box.reset();
        if (!arena_.try_close()) std::terminate();
    }

    // Called ONLY by the trusted scheduler with supervisor-authenticated
    // ActorHeapIdentity. Knowing the numeric tuple is not sufficient proof
    // of caller authority; this helper provides no guest-callable bind API.
    bool bind_carrier(ActorHeapIdentity claimed) noexcept {
        if (!identity_.runtime || !identity_.epoch || claimed != identity_ ||
                shutting_down_.load(std::memory_order_acquire)) return false;
        std::lock_guard<std::mutex> lock(bind_lock_);
        if (shutting_down_.load(std::memory_order_acquire)) return false;
        if (!bound_.load(std::memory_order_relaxed)) {
            carrier_ = std::this_thread::get_id();
            bound_.store(true, std::memory_order_release);
        }
        return on_actor_carrier(claimed);
    }

    std::uint64_t arena_domain() const noexcept { return arena_.domain(); }

    template <typename... Args>
    bool emplace_at(ActorHeapIdentity actor, std::size_t slot, Args&&... args) noexcept
        requires std::is_nothrow_constructible_v<T, Args...>
    {
        if (!on_actor_carrier(actor) || retired_ || executing_callback_ ||
                dropping_ || slot >= Slots || live_[slot].is_owned())
            return false;
        auto ticket = arena_.issue_at(slot, std::forward<Args>(args)...);
        if (!ticket.valid()) return false;
        return UniqueBox<T>::adopt(ticket, live_[slot]);
    }

    // A reference is usable only during a synchronous exclusive callback.
    // A guest callback may not retain a raw reference beyond this scope:
    // borrow checker must enforce that separately.
    template <typename F>
    bool with_at(ActorHeapIdentity actor, std::size_t slot, F&& callback) {
        if (!on_actor_carrier(actor) || retired_ || executing_callback_ ||
                dropping_ || slot >= Slots || !live_[slot].is_owned())
            return false;
        auto* value = live_[slot].get_checked(arena_.domain(),
                                             live_[slot].owner_generation());
        if (!value || !arena_.is_current_owner(slot, live_[slot].owner_domain(),
                live_[slot].owner_generation(), value)) return false;
        executing_callback_ = true;
        struct Guard { bool& flag; ~Guard() noexcept { flag = false; } } guard{executing_callback_};
        std::forward<F>(callback)(*value);
        return true;
    }

    bool drop_at(ActorHeapIdentity actor, std::size_t slot) noexcept {
        if (!on_actor_carrier(actor) || retired_ || executing_callback_ ||
                dropping_ || slot >= Slots || !live_[slot].is_owned())
            return false;
        const auto* value = live_[slot].get_checked(arena_.domain(),
                                                      live_[slot].owner_generation());
        if (!value || !arena_.is_current_owner(slot, live_[slot].owner_domain(),
                live_[slot].owner_generation(), value)) return false;
        dropping_ = true;
        live_[slot].reset();
        dropping_ = false;
        return true;
    }

    // The supervisor must have already fenced actor turns, in-flight I/O,
    // FFI and pinned continuations. This bridge checks only LOCAL callbacks
    // and all owned boxes; it cannot prove external scheduler quiescence.
    bool retire_drained(ActorHeapIdentity actor) noexcept {
        if (!on_actor_carrier(actor) || executing_callback_ || dropping_) return false;
        if (retired_) return true;
        for (auto& box : live_) if (box.is_owned()) return false;
        if (!arena_.try_close()) return false;
        retired_ = true;
        return true;
    }
};

} // namespace oreslang::runtime::native_ownership_v1
