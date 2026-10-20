#pragma once

#include "oreslang/runtime/ActorHeapRetirementProof.hpp"
#include "oreslang/runtime/ManagedRef.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <optional>
#include <stdexcept>

namespace oreslang::runtime {

namespace detail {
// One namespace for all specializations; never wrap/reissue arena identities.
inline std::atomic<std::uint64_t> next_managed_slot_arena_id{1};

inline std::uint64_t reserve_managed_slot_arena_id(
        std::atomic<std::uint64_t>& next) {
    auto candidate = next.load(std::memory_order_relaxed);
    while (candidate != 0 && candidate != std::numeric_limits<std::uint64_t>::max()) {
        if (next.compare_exchange_weak(candidate, candidate + 1,
                std::memory_order_relaxed, std::memory_order_relaxed)) {
            return candidate;
        }
    }
    throw std::overflow_error("managed arena identity space exhausted");
}
} // namespace detail

/**
 * Bounded actor-owned storage proof for the 32-bit managed-reference layout.
 *
 * Real byte storage, not a synthetic virtual base: reference fields encode an
 * offset into independently allocated actor storage, whereas native64 stores
 * native addresses. Both layouts carry identical owner/generation checks.
 *
 * No raw native address leaves this API. Its public Handle is a trusted host
 * descriptor, NOT the 4-byte guest representation. Direct guest references
 * additionally require compiler-verified provenance/borrows and inbound-edge
 * invalidation before slot reuse. See docs/MANAGED_REFERENCE_LAYOUT.md.
 *
 * This is a fixed-size POD byte-slot prototype, not a guest object allocator:
 * it does not run destructors, enforce CPU isolation, or implement ownership
 * checking for arbitrary Oreslang object graphs.
 */
template<std::size_t SlotCount, std::size_t PayloadBytes,
         ManagedRefMode Mode = kDefaultManagedRefMode>
class ActorManagedSlotArena final {
    static_assert(SlotCount > 0 && PayloadBytes > 0, "arena needs nonzero capacity");
    static_assert(PayloadBytes <= std::numeric_limits<std::size_t>::max() - 7,
                  "slot size overflows the aligned layout");
    static constexpr std::size_t kStride = (PayloadBytes + 7) & ~std::size_t{7};
    static_assert(SlotCount <= std::numeric_limits<std::size_t>::max() / kStride,
                  "arena byte size overflows size_t");
    static constexpr std::size_t kTotalBytes = SlotCount * kStride;
    static_assert(Mode != ManagedRefMode::Compressed32 ||
                  ((SlotCount - 1) <= CompressedActorRef::max_offset / kStride),
                  "actor slot start exceeds compressed32 offset range");

public:
    using Ref = ActorManagedRef<Mode>;
    static_assert(Mode != ManagedRefMode::Compressed32 || sizeof(Ref) == 4,
                  "compressed guest references must occupy four bytes");
    static_assert(Mode != ManagedRefMode::Native64 || sizeof(Ref) == 8,
                  "native managed references must occupy eight bytes");

    struct Handle final {
        std::uint64_t arena;
        Ref reference;
        std::uint64_t generation;
    };

private:
    struct Slot final {
        std::uint64_t generation = 1;
        std::size_t length = 0;
        bool live = false;
    };

    ActorHeapIdentity owner_;
    std::uint64_t arena_id_;
    ActorHeapBorrowGate gate_;
    alignas(8) std::array<std::byte, kTotalBytes> storage_{};
    std::array<Slot, SlotCount> slots_{};
    bool retired_ = false;

    std::uintptr_t base() const noexcept {
        return reinterpret_cast<std::uintptr_t>(storage_.data());
    }

    void require_open(ActorHeapIdentity who) const {
        if (who != owner_) throw std::logic_error("foreign actor managed arena");
        if (retired_ || gate_.sealed())
            throw std::logic_error("managed arena is retired or retiring");
    }

    std::size_t checked_slot(Handle handle) const {
        if (handle.arena != arena_id_)
            throw std::logic_error("foreign managed arena handle");
        const auto decoded = handle.reference.decode(base(), kTotalBytes);
        if (!decoded || *decoded == 0 || *decoded < base())
            throw std::logic_error("invalid managed reference encoding");
        const auto offset = *decoded - base();
        if (offset % kStride != 0)
            throw std::logic_error("managed reference is not an allocation start");
        const auto index = offset / kStride;
        if (index >= SlotCount || !slots_[index].live ||
                slots_[index].generation != handle.generation)
            throw std::logic_error("stale managed arena handle");
        return index;
    }

    std::byte* payload(std::size_t index) noexcept {
        return storage_.data() + index * kStride;
    }
    const std::byte* payload(std::size_t index) const noexcept {
        return storage_.data() + index * kStride;
    }

public:
    explicit ActorManagedSlotArena(ActorHeapIdentity owner)
        : owner_(owner),
          arena_id_(detail::reserve_managed_slot_arena_id(
              detail::next_managed_slot_arena_id)), gate_(owner) {
        if (!owner.runtime || !owner.actor || !owner.epoch)
            throw std::invalid_argument("invalid actor heap identity");
    }

    ActorManagedSlotArena(const ActorManagedSlotArena&) = delete;
    ActorManagedSlotArena& operator=(const ActorManagedSlotArena&) = delete;
    ActorManagedSlotArena(ActorManagedSlotArena&&) = delete;
    ActorManagedSlotArena& operator=(ActorManagedSlotArena&&) = delete;

    ~ActorManagedSlotArena() noexcept {
        // External Lease stores a pointer to gate_ itself. Destruction with
        // an outstanding lease would let an off-carrier release touch freed
        // memory. Seal first; fail closed rather than causing a silent UAF.
        // The owner must separately prove all actor turns have quiesced.
        if (!gate_.seal_and_quiesced(owner_)) std::terminate();
        std::fill(storage_.begin(), storage_.end(), std::byte{0});
    }

    static constexpr std::size_t guest_reference_bytes() noexcept { return sizeof(Ref); }
    static constexpr std::size_t slot_stride() noexcept { return kStride; }
    static constexpr std::size_t arena_bytes() noexcept { return kTotalBytes; }
    std::uint64_t arena_id() const noexcept { return arena_id_; }
    bool retired() const noexcept { return retired_; }

    std::optional<Handle> allocate(ActorHeapIdentity who, const void* source,
                                   std::size_t length) {
        require_open(who);
        if (!length || length > PayloadBytes || source == nullptr)
            throw std::invalid_argument("invalid managed object byte payload");
        for (std::size_t i = 0; i < SlotCount; ++i) {
            auto& slot = slots_[i];
            if (slot.live || slot.generation == std::numeric_limits<std::uint64_t>::max())
                continue;
            const auto location = base() + i * kStride;
            const auto encoded = Ref::encode(base(), kTotalBytes, location);
            if (!encoded) throw std::logic_error("actor arena layout cannot encode slot");
            std::fill(payload(i), payload(i) + kStride, std::byte{0});
            std::memcpy(payload(i), source, length);
            slot.length = length;
            slot.live = true;
            return Handle{arena_id_, *encoded, slot.generation};
        }
        return std::nullopt;
    }

    std::size_t copy_out(ActorHeapIdentity who, Handle handle,
                         void* destination, std::size_t capacity) const {
        require_open(who);
        const auto i = checked_slot(handle);
        if (capacity < slots_[i].length || destination == nullptr)
            throw std::invalid_argument("managed payload output is too small");
        std::memcpy(destination, payload(i), slots_[i].length);
        return slots_[i].length;
    }

    void write(ActorHeapIdentity who, Handle handle,
               const void* source, std::size_t length) {
        require_open(who);
        const auto i = checked_slot(handle);
        if (!length || length > PayloadBytes || source == nullptr)
            throw std::invalid_argument("invalid managed payload replacement");
        std::fill(payload(i), payload(i) + kStride, std::byte{0});
        std::memcpy(payload(i), source, length);
        slots_[i].length = length;
    }

    void release(ActorHeapIdentity who, Handle handle) {
        require_open(who);
        const auto i = checked_slot(handle);
        if (slots_[i].generation == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("managed slot generation exhausted");
        std::fill(payload(i), payload(i) + kStride, std::byte{0});
        slots_[i].length = 0;
        slots_[i].live = false;
        ++slots_[i].generation;
    }

    // Lifetime lease for native continuations/FFI. Holding it never grants
    // permission to dereference a compressed guest reference.
    std::optional<ActorHeapBorrowGate::Lease> borrow(ActorHeapIdentity who) {
        require_open(who);
        return gate_.acquire(who);
    }

    void retire(ActorHeapIdentity who, const CarrierRetirementProof& proof) {
        if (who != owner_ || proof.owner() != owner_ || retired_)
            throw std::logic_error("managed arena retirement proof mismatch");
        if (!gate_.seal_and_quiesced(who))
            throw std::logic_error("managed arena has outstanding lifetime leases");
        // Gate is sealed even if retirement initially failed; callers can
        // retry ONLY retirement once external leases complete.
        std::fill(storage_.begin(), storage_.end(), std::byte{0});
        for (auto& slot : slots_) {
            slot.live = false;
            slot.length = 0;
            if (slot.generation < std::numeric_limits<std::uint64_t>::max())
                ++slot.generation;
        }
        retired_ = true;
    }
};

} // namespace oreslang::runtime
