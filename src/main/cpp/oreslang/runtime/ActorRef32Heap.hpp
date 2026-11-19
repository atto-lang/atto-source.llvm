#pragma once
// AttoLang actor-local generational guest handles: experimental HOST adapter.
// Stacked on M2b ActorSlabHeap32. Not installed into guest ABI v4 or emitted
// from general .atto compiler code; raw Offset32 remains ABI v4. No host sandbox.
// C++17, bounded preallocated sidecars; NEVER transport bare guest tokens across actors.
// A host mutex serializes access vs concurrent scheduler carriers; this is NOT
// a substitute for compiled critical-read/write and suspension loan checking.
#include "ActorSlabHeap32.hpp"
#include <cstdint>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace oreslang::runtime::vm {
struct ActorHeapOwner {
    std::uint64_t tenant, actor, incarnation;
    friend bool operator==(ActorHeapOwner a, ActorHeapOwner b) noexcept {
        return a.tenant == b.tenant && a.actor == b.actor &&
               a.incarnation == b.incarnation;
    }
};

// In a compiled guest only the 4-byte token is stored. The typed compiler and
// trusted host must preserve origin separately; a token by itself is NOT a
// cross-actor capability and equal numeric tokens may exist in sibling heaps.
// A TRUSTED HOST witness (not the four-byte guest representation).
// Only the owning arena may mint one. Untrusted guest code cannot fabricate
// an origin by passing {token, owner} to public read/write APIs.
class TrustedActorRef32 final {
public:
    TrustedActorRef32() noexcept = default; // invalid/null reference
    std::uint32_t guest_token() const noexcept { return token_; }
    ActorHeapOwner origin() const noexcept { return origin_; }
private:
    friend class ActorRef32Heap;
    TrustedActorRef32(std::uint32_t token, ActorHeapOwner origin) noexcept
        : token_(token), origin_(origin) {}
    std::uint32_t token_ = 0;
    ActorHeapOwner origin_{0, 0, 0};
};
static_assert(sizeof(std::uint32_t) == 4,
              "portable guest reference token is exactly 4 bytes");

class ActorRef32Heap final {
public:
    using Offset32 = ActorSlabHeap32::Offset32;
    static constexpr std::uint32_t kSlotBits = 20;
    static constexpr std::uint32_t kSlotMask = (1u << kSlotBits) - 1u;
    static constexpr std::uint32_t kMaxGeneration = (1u << (32 - kSlotBits)) - 1u;

    ActorRef32Heap(ActorHeapOwner owner, std::uint8_t* cage, Offset32 base,
                   Offset32 capacity, std::uint32_t slot_limit)
        : owner_(owner), cage_(cage), slab_(capacity), slots_(checked_slots(slot_limit)) {
        slab_.bind(cage, base);
    }
    ActorRef32Heap(const ActorRef32Heap&) = delete;
    ActorRef32Heap& operator=(const ActorRef32Heap&) = delete;

    TrustedActorRef32 allocate(ActorHeapOwner caller, Offset32 size,
                                Offset32 alignment, std::uint32_t type_id) {
        const std::lock_guard<std::mutex> guard(mutex_);
        if (!authorized(caller) || type_id == 0 || size == 0) return {};
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            Slot& slot = slots_[i];
            if (slot.live || slot.retired) continue;
            const Offset32 offset = slab_.allocate(size, alignment);
            if (!offset) return {};
            slot.offset = offset;
            slot.size = size;
            slot.alignment = alignment;
            slot.type_id = type_id;
            slot.live = true;
            return TrustedActorRef32{ (std::uint32_t(slot.generation) << kSlotBits) |
                                         std::uint32_t(i + 1), owner_ };
        }
        return {};
    }

    bool write_i64(TrustedActorRef32 ref, ActorHeapOwner caller,
                   std::uint32_t type_id, Offset32 displacement,
                   std::int64_t value) {
        const std::lock_guard<std::mutex> guard(mutex_);
        const Slot* slot = checked(ref, caller, type_id, displacement, 8);
        if (!slot) return false;
        std::memcpy(cage_ + slot->offset + displacement, &value, sizeof(value));
        return true;
    }

    bool read_i64(TrustedActorRef32 ref, ActorHeapOwner caller,
                  std::uint32_t type_id, Offset32 displacement,
                  std::int64_t& out) const {
        const std::lock_guard<std::mutex> guard(mutex_);
        const Slot* slot = checked(ref, caller, type_id, displacement, 8);
        if (!slot) return false;
        std::memcpy(&out, cage_ + slot->offset + displacement, sizeof(out));
        return true;
    }

    bool release(TrustedActorRef32 ref, ActorHeapOwner caller,
                 std::uint32_t type_id) {
        const std::lock_guard<std::mutex> guard(mutex_);
        Slot* slot = checked_slot(ref, caller, type_id);
        if (!slot || !slab_.release(slot->offset, slot->size, slot->alignment))
            return false;
        slot->live = false;
        if (slot->generation == kMaxGeneration) slot->retired = true;
        else ++slot->generation;
        slot->offset = slot->size = slot->alignment = slot->type_id = 0;
        return true;
    }

    // Only after the owner actor's loans, callbacks and frames are quiescent.
    // The underlying slab release scrubs each live allocation before reset.
    bool close_after_quiescence() {
        const std::lock_guard<std::mutex> guard(mutex_);
        if (closed_) return true;
        for (Slot& slot : slots_) {
            if (slot.live) {
                if (!slab_.release(slot.offset, slot.size, slot.alignment))
                    return false;
                slot.live = false;
            }
            slot.retired = true;
        }
        slab_.reset();
        closed_ = true;
        return true;
    }

    bool closed() const {
        const std::lock_guard<std::mutex> guard(mutex_);
        return closed_;
    }
    std::size_t sidecar_bytes() const {
        const std::lock_guard<std::mutex> guard(mutex_);
        return slots_.capacity() * sizeof(Slot) + slab_.metadata_bytes();
    }

private:
    struct Slot {
        Offset32 offset = 0, size = 0, alignment = 0;
        std::uint32_t type_id = 0;
        std::uint16_t generation = 1;
        bool live = false, retired = false;
    };
    static std::size_t checked_slots(std::uint32_t n) {
        if (n == 0 || n > kSlotMask)
            throw std::invalid_argument("Ref32 slot count out of bounds");
        return n;
    }
    bool authorized(ActorHeapOwner caller) const noexcept {
        return !closed_ && caller == owner_;
    }
    Slot* checked_slot(TrustedActorRef32 ref, ActorHeapOwner caller,
                       std::uint32_t type_id) noexcept {
        return const_cast<Slot*>(
            static_cast<const ActorRef32Heap*>(this)->checked_slot(ref, caller, type_id));
    }
    const Slot* checked_slot(TrustedActorRef32 ref, ActorHeapOwner caller,
                             std::uint32_t type_id) const noexcept {
        if (!authorized(caller) || !(ref.origin() == owner_) || !ref.guest_token() || !type_id)
            return nullptr;
        const std::uint32_t i = (ref.guest_token() & kSlotMask);
        const std::uint32_t generation = ref.guest_token() >> kSlotBits;
        if (!i || i > slots_.size() || !generation) return nullptr;
        const Slot& slot = slots_[i - 1];
        if (!slot.live || slot.retired || slot.generation != generation ||
            slot.type_id != type_id) return nullptr;
        return &slot;
    }
    const Slot* checked(TrustedActorRef32 ref, ActorHeapOwner caller,
                        std::uint32_t type_id, Offset32 displacement,
                        Offset32 width) const noexcept {
        const Slot* slot = checked_slot(ref, caller, type_id);
        if (!slot || (displacement & 7u) || slot->offset % 8 != 0 ||
            displacement > slot->size || width > slot->size - displacement)
            return nullptr;
        return slot;
    }

    ActorHeapOwner owner_;
    std::uint8_t* cage_;
    ActorSlabHeap32 slab_;
    std::vector<Slot> slots_;
    bool closed_ = false;
    mutable std::mutex mutex_; // protects sidecars, slab and retirement vs carrier threads
};
} // namespace oreslang::runtime::vm
