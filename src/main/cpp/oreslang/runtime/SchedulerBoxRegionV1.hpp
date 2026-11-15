#pragma once
// Trusted host-only Box<T> bridge to the REAL native actor scheduler.
// NOT a source-language Box ABI: the guest cannot construct this object,
// mint a scheduler ref or retain pointers returned inside callbacks.
#include "oreslang/runtime/NativeActorScheduler.h"
#include "oreslang/runtime/NativeBoxArenaV1.hpp"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <type_traits>
#include <utility>

namespace oreslang::runtime::native_ownership_v1 {

template <typename T, std::size_t Slots>
class SchedulerBoxRegionV1 final {
    static_assert(Slots > 0, "scheduler Box region requires at least one slot");
    OresNativeActorScheduler* scheduler_;
    const OresNativeActorRef actor_;
    FixedBoxArena<T, Slots> arena_{};
    std::array<UniqueBox<T>, Slots> boxes_{};
    // Actor-local fields: the scheduler guarantees one physical turn per
    // exact actor incarnation, even when successive turns migrate workers.
    // The external retire path runs only AFTER wait_done's quiescence fence.
    // A single supervisor wins retirement; losers and reentrant drops
    // fail immediately, without waiting or attempting a second destruction.
    std::atomic<bool> retiring_{false};
    std::atomic<bool> retired_{false};
    bool borrowed_ = false;
    bool dropping_ = false;

    bool in_authenticated_turn() const noexcept {
        return scheduler_ && actor_.scheduler_domain != 0 &&
            actor_.generation != 0 &&
            !retired_.load(std::memory_order_acquire) &&
            ores_native_actor_scheduler_is_current_actor(scheduler_, actor_) != 0;
    }

    T* verified_value(std::size_t slot) noexcept {
        if (slot >= Slots || !boxes_[slot].is_owned()) return nullptr;
        auto& box = boxes_[slot];
        auto* ptr = box.get_checked(arena_.domain(), box.owner_generation());
        if (!ptr || !arena_.is_current_owner(
                slot, box.owner_domain(), box.owner_generation(), ptr))
            return nullptr;
        return ptr;
    }

public:
    // Caller must supply a live scheduler and a ref minted by its spawn API.
    // The scheduler MUST outlive this region; never expose it to guest code.
    SchedulerBoxRegionV1(OresNativeActorScheduler* scheduler,
                         OresNativeActorRef actor) noexcept
        : scheduler_(scheduler), actor_(actor) {}
    SchedulerBoxRegionV1(const SchedulerBoxRegionV1&) = delete;
    SchedulerBoxRegionV1& operator=(const SchedulerBoxRegionV1&) = delete;
    SchedulerBoxRegionV1(SchedulerBoxRegionV1&&) = delete;
    SchedulerBoxRegionV1& operator=(SchedulerBoxRegionV1&&) = delete;

    ~SchedulerBoxRegionV1() noexcept {
        // Shutdown MUST be completed by the external supervisor, not by
        // a destructor racing an active or parked actor continuation.
        if (!retired_.load(std::memory_order_acquire) ||
                borrowed_ || dropping_ || !arena_.try_close())
            std::terminate();
    }

    template <typename... Args>
    bool emplace_at(std::size_t slot, Args&&... args) noexcept
        requires std::is_nothrow_constructible_v<T, Args...>
    {
        if (!in_authenticated_turn() || borrowed_ || dropping_ ||
                slot >= Slots || boxes_[slot].is_owned()) return false;
        auto ticket = arena_.issue_at(slot, std::forward<Args>(args)...);
        return ticket.valid() && UniqueBox<T>::adopt(ticket, boxes_[slot]);
    }

    // Callback stays on the scheduler's authenticated actor turn. Compiler
    // lowering must still forbid guest suspension or raw pointer escape.
    template <typename F>
    bool with_at(std::size_t slot, F&& callback) {
        if (!in_authenticated_turn() || borrowed_ || dropping_) return false;
        T* ptr = verified_value(slot);
        if (!ptr) return false;
        borrowed_ = true;
        struct Guard { bool& active; ~Guard() noexcept { active = false; } }
            guard{borrowed_};
        std::forward<F>(callback)(*ptr);
        return true;
    }

    bool drop_at(std::size_t slot) noexcept {
        if (!in_authenticated_turn() || borrowed_ || dropping_ ||
                !verified_value(slot)) return false;
        dropping_ = true;
        boxes_[slot].reset();
        dropping_ = false;
        return true;
    }

    // Host-only blocking STOP/panic/cancellation reclamation path.
    // Caller must already have sealed/stopped this EXACT actor incarnation
    // and fenced external producers; never call on a scheduler carrier.
    // wait_done also waits for retained external resource loans and callbacks.
    bool retire_after_done() noexcept {
        if (!scheduler_ ||
                ores_native_actor_scheduler_is_current_carrier(scheduler_))
            return false;
        bool expected = false;
        if (!retiring_.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel,
                std::memory_order_acquire))
            return false; // duplicate or reentrant retirement
        if (!ores_native_actor_scheduler_wait_done(scheduler_, actor_)) {
            retiring_.store(false, std::memory_order_release);
            return false; // STOP not yet sealed, or stale scheduler generation
        }
        // wait_done fences every active callback and retained resource loan.
        // Only now is reading the actor-local callback/drop state safe.
        if (borrowed_ || dropping_) std::terminate();
        retired_.store(true, std::memory_order_release);
        dropping_ = true;
        for (auto& box : boxes_) box.reset();
        dropping_ = false;
        return arena_.try_close();
    }

    bool is_retired() const noexcept {
        return retired_.load(std::memory_order_acquire);
    }
};

} // namespace oreslang::runtime::native_ownership_v1
