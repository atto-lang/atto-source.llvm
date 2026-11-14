#pragma once

#include "oreslang/runtime/NativeAtomicBoxV1.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <new>
#include <type_traits>
#include <utility>

namespace oreslang::runtime::native_ownership_v1 {

// Unique across all T/slot-count specializations in this native process.
// A domain value is an identity, not a security capability; actor isolation
// must additionally consult the execution carrier's trusted authorization.
inline std::atomic<std::uint64_t> next_box_arena_domain{1};

// Fixed-slot allocator with no GC, heap allocation, OS locks, or hidden
// fallback. The allocator itself controls physical allocation identity,
// ticket issuance, slot generation and deterministic destructor execution.
// The arena MUST outlive every outstanding ticket/box. Its destruction
// terminates on live owners rather than allowing silent use-after-free.
template <typename T, std::size_t Slots>
class FixedBoxArena final {
    static_assert(Slots > 0, "Box arena must contain slots");
    static_assert(std::is_nothrow_destructible_v<T>,
                  "Box destructor must be noexcept");

    // Each slot has exactly one of: free, constructing, live, dropping,
    // permanently exhausted. Only free -> constructing can issue a ticket.
    static constexpr std::uint8_t kFree = 0;
    static constexpr std::uint8_t kConstructing = 1;
    static constexpr std::uint8_t kLive = 2;
    static constexpr std::uint8_t kDropping = 3;
    static constexpr std::uint8_t kExhausted = 4;

    struct Slot {
        alignas(T) std::byte storage[sizeof(T)]{};
        std::atomic<std::uint8_t> state{kFree};
        std::atomic<std::uint32_t> generation{1};
    };

    std::array<Slot, Slots> slots_{};
    std::uint64_t domain_ = next_box_arena_domain.fetch_add(
        1, std::memory_order_relaxed);
    std::atomic<bool> closing_{false};

    static void drop_slot(void* context, T* value) noexcept {
        if (!context || !value) std::terminate();
        auto& slot = *static_cast<Slot*>(context);
        auto* actual = std::launder(reinterpret_cast<T*>(slot.storage));
        if (actual != value) std::terminate();
        if (slot.state.exchange(kDropping, std::memory_order_acq_rel) != kLive)
            std::terminate(); // duplicate drop or corrupt trusted ticket
        value->~T();
        const auto generation = slot.generation.load(std::memory_order_relaxed);
        // Never wrap generation and revalidate an old owner.
        if (generation == std::numeric_limits<std::uint32_t>::max()) {
            slot.state.store(kExhausted, std::memory_order_release);
        } else {
            slot.generation.store(generation + 1, std::memory_order_relaxed);
            slot.state.store(kFree, std::memory_order_release);
        }
    }

public:
    FixedBoxArena() noexcept {
        if (domain_ == 0 || domain_ == std::numeric_limits<std::uint64_t>::max())
            std::terminate(); // no repeated allocation domain identity
    }
    ~FixedBoxArena() noexcept {
        closing_.store(true, std::memory_order_release);
        for (const auto& slot : slots_) {
            const auto state = slot.state.load(std::memory_order_acquire);
            if (state != kFree && state != kExhausted) std::terminate();
        }
    }
    FixedBoxArena(const FixedBoxArena&) = delete;
    FixedBoxArena& operator=(const FixedBoxArena&) = delete;
    FixedBoxArena(FixedBoxArena&&) = delete;
    FixedBoxArena& operator=(FixedBoxArena&&) = delete;

    std::uint64_t domain() const noexcept { return domain_; }
    static constexpr std::size_t capacity() noexcept { return Slots; }

    // Construction is guaranteed nothrow. A busy, exhausted, retired or
    // out-of-range slot produces an invalid ticket without touching T.
    // Returning a nonmovable ticket relies on C++17+ guaranteed copy elision.
    template <typename... Args>
    UniqueAllocationTicket<T> issue_at(std::size_t index, Args&&... args) noexcept
        requires std::is_nothrow_constructible_v<T, Args...>
    {
        if (index >= Slots || closing_.load(std::memory_order_acquire))
            return UniqueAllocationTicket<T>{};
        auto& slot = slots_[index];
        std::uint8_t expected = kFree;
        if (!slot.state.compare_exchange_strong(
                expected, kConstructing, std::memory_order_acq_rel,
                std::memory_order_acquire))
            return UniqueAllocationTicket<T>{};
        if (closing_.load(std::memory_order_acquire)) {
            slot.state.store(kFree, std::memory_order_release);
            return UniqueAllocationTicket<T>{};
        }
        const auto generation = slot.generation.load(std::memory_order_relaxed);
        if (!generation) std::terminate();
        T* value = ::new (static_cast<void*>(slot.storage))
            T{std::forward<Args>(args)...};
        slot.state.store(kLive, std::memory_order_release);
        return UniqueAllocationTicket<T>(
            value, static_cast<void*>(&slot), &drop_slot, domain_, generation);
    }

    bool try_close() noexcept {
        // One-way admission fence; outstanding boxes may still be released.
        closing_.store(true, std::memory_order_release);
        for (const auto& slot : slots_) {
            const auto state = slot.state.load(std::memory_order_acquire);
            if (state != kFree && state != kExhausted) return false;
        }
        return true;
    }
};

} // namespace oreslang::runtime::native_ownership_v1
