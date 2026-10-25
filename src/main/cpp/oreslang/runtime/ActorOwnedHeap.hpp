#pragma once

#include "oreslang/runtime/ActorHeapIdentity.hpp"
#include "oreslang/runtime/ManagedRef.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <utility>
#include <limits>
#include <optional>
#include <stdexcept>
#include <type_traits>

// Prototype for actor-owned, NON-GC native storage. This is a fixed-capacity
// byte heap, not yet compiler-lowered Oreslang guest objects or a sandbox.
// Actor execution must externally enforce exclusive access and quiescence.
namespace oreslang::runtime {

// All actor byte-heap layouts share a non-wrapping identity space. A
// fetch_add-based allocator can overflow through UINT64_MAX back to 1,
// resurrecting a stale handle even after reporting exhaustion. The terminal
// counter value is never advanced or reused.
namespace detail {
inline std::atomic<std::uint64_t> next_actor_owned_heap_id{1};

inline std::uint64_t reserve_actor_owned_heap_id(
        std::atomic<std::uint64_t>& next) {
    auto current = next.load(std::memory_order_relaxed);
    while (current != 0 && current != std::numeric_limits<std::uint64_t>::max()) {
        if (next.compare_exchange_weak(current, current + 1,
                std::memory_order_relaxed, std::memory_order_relaxed))
            return current;
    }
    throw std::overflow_error("actor heap identity exhausted");
}
} // namespace detail

template<std::size_t Bytes, std::size_t Slots>
class ActorOwnedHeap final {
    static_assert(Bytes > 0 && Slots > 0, "bounded positive heap required");

public:
    class Handle final {
        friend class ActorOwnedHeap;
        std::uint64_t heap_ = 0;
        std::uint64_t incarnation_ = 0;
        std::size_t slot_ = 0;
        std::uint64_t generation_ = 0;

        Handle(std::uint64_t heap, std::uint64_t incarnation,
               std::size_t slot, std::uint64_t generation) noexcept
            : heap_(heap), incarnation_(incarnation),
              slot_(slot), generation_(generation) {}
    public:
        Handle() = default; // default handle is invalid
        bool operator==(const Handle& rhs) const noexcept {
            return heap_ == rhs.heap_ && incarnation_ == rhs.incarnation_
                && slot_ == rhs.slot_ && generation_ == rhs.generation_;
        }
    };

private:
    struct Slot {
        std::size_t offset = 0;
        std::size_t length = 0;
        // Physical allocation extent starts here, including its alignment
        // padding. Slots own the entire extent until deterministic drop.
        std::size_t rewind_to = 0;
        std::uint64_t generation = 1;
        bool live = false;
    };

    struct Hole {
        std::size_t offset = 0;
        std::size_t length = 0;
    };

    // The byte array itself lives in native memory, not a Java object heap.
    alignas(std::max_align_t) std::array<std::byte, Bytes> memory_{};
    std::array<Slot, Slots> slots_{};
    std::array<std::size_t, Slots> free_slots_{};
    // One authoritative sorted free-range table, statically bounded and
    // never requiring malloc/GC during an actor allocation or drop.
    std::array<Hole, Slots + 1> holes_{};
    std::size_t hole_count_ = 0;
    std::size_t next_slot_ = 0;
    std::size_t free_count_ = 0;
    std::size_t used_ = 0;
    std::size_t live_count_ = 0;
    const std::uint64_t actor_id_;
    const OresNativeAllocationDomainRef allocation_domain_;
    const std::uint64_t heap_identity_;
    std::uint64_t incarnation_ = 1;
    bool retired_ = false;

    static std::uint64_t new_identity() {
        return detail::reserve_actor_owned_heap_id(detail::next_actor_owned_heap_id);
    }

    // Insert an owned extent after deterministic destruction, coalescing
    // adjacent holes and rewinding an empty bump tail. All live allocations
    // remain at their original stable addresses.
    void recycle_extent(std::size_t start, std::size_t end) noexcept {
        if (end == used_) {
            used_ = start;
        } else {
            std::size_t i = 0;
            while (i < hole_count_ && holes_[i].offset < start) ++i;
            const bool merge_before = i > 0
                && holes_[i - 1].offset + holes_[i - 1].length == start;
            const bool merge_after = i < hole_count_
                && end == holes_[i].offset;
            if (merge_before && merge_after) {
                holes_[i - 1].length += end - start + holes_[i].length;
                std::move(holes_.begin() + i + 1, holes_.begin() + hole_count_,
                          holes_.begin() + i);
                --hole_count_;
            } else if (merge_before) {
                holes_[i - 1].length += end - start;
            } else if (merge_after) {
                holes_[i].offset = start;
                holes_[i].length += end - start;
            } else {
                // With at most Slots live objects and extents kept disjoint,
                // the number of free interior ranges is <= Slots + 1.
                if (hole_count_ == holes_.size()) std::terminate();
                std::move_backward(holes_.begin() + i,
                                   holes_.begin() + hole_count_,
                                   holes_.begin() + hole_count_ + 1);
                holes_[i] = Hole{start, end - start};
                ++hole_count_;
            }
        }
        while (hole_count_ > 0) {
            const Hole tail = holes_[hole_count_ - 1];
            if (tail.offset + tail.length != used_) break;
            used_ = tail.offset;
            --hole_count_;
        }
    }

    bool valid(const Handle& handle) const noexcept {
        if (retired_ || handle.heap_ != heap_identity_
                || handle.incarnation_ != incarnation_
                || handle.slot_ >= next_slot_)
            return false;
        const auto& slot = slots_[handle.slot_];
        return slot.live && slot.generation == handle.generation_;
    }

public:
    /*
     * Preferred runtime constructor: heap ownership is the authenticated actor
     * allocation capability, never the transient logical execution id.
     */
    explicit ActorOwnedHeap(OresNativeAllocationDomainRef allocation_domain)
        : actor_id_(allocation_domain.owner),
          allocation_domain_(allocation_domain),
          heap_identity_(new_identity()) {
        if (allocation_domain.kind != ORES_ALLOCATION_DOMAIN_ACTOR
                || allocation_domain.domain == 0
                || allocation_domain.generation == 0)
            throw std::invalid_argument("invalid actor allocation domain");
    }

    explicit ActorOwnedHeap(ActorHeapIdentity identity)
        : ActorOwnedHeap(actor_allocation_domain(identity)) {}

    /*
     * Legacy host-only prototype constructor. It intentionally carries no
     * authenticated runtime allocation capability; new runtime integrations
     * must use ActorHeapIdentity or OresNativeAllocationDomainRef.
     */
    explicit ActorOwnedHeap(std::uint64_t actor_id)
        : actor_id_(actor_id),
          allocation_domain_{ORES_ALLOCATION_DOMAIN_NONE, 0, 0, 0},
          heap_identity_(new_identity()) {
        if (actor_id == 0) throw std::invalid_argument("zero actor id");
    }

    ActorOwnedHeap(const ActorOwnedHeap&) = delete;
    ActorOwnedHeap& operator=(const ActorOwnedHeap&) = delete;
    ActorOwnedHeap(ActorOwnedHeap&&) = delete;
    ActorOwnedHeap& operator=(ActorOwnedHeap&&) = delete;

    ~ActorOwnedHeap() noexcept {
        // Defense in depth for host teardown paths that destroy a heap object
        // after scheduler quiescence but before explicit retire_quiesced().
        // This does not replace the retirement proof; it only prevents guest
        // payload bytes from remaining in reclaimed host storage.
        std::fill(memory_.begin(), memory_.end(), std::byte{0});
    }

    std::uint64_t actor_id() const noexcept { return actor_id_; }
    OresNativeAllocationDomainRef allocation_domain() const noexcept {
        return allocation_domain_;
    }
    bool allocation_domain_bound() const noexcept {
        return allocation_domain_.kind == ORES_ALLOCATION_DOMAIN_ACTOR;
    }
    std::size_t used() const noexcept { return used_; }
    std::size_t live_allocations() const noexcept { return live_count_; }
    bool retired() const noexcept { return retired_; }
    static constexpr std::size_t capacity = Bytes;

    // 4-byte actor-relative location for compiler-managed object fields. A
    // CompactRef is NOT a capability: it contains no actor/heap identity,
    // generation or authorization. Never send it to another actor, put it on
    // the wire, or decode it without the original lifetime/provenance proof.
    // The native FFI and ActorRef ABI remain pointer-width independent.
    using CompactRef = CompressedActorRef;
    static_assert(sizeof(CompactRef) == 4, "actor-local edge must occupy four bytes");

    // Turn a currently live, generationally checked heap allocation into a
    // compact local offset. Only 8-byte-aligned object starts are representable
    // in this mode; other alignments must use a native64/handle representation.
    std::optional<CompactRef> compact(const Handle& handle) const noexcept {
        if (!valid(handle)) return std::nullopt;
        const auto base = reinterpret_cast<std::uintptr_t>(memory_.data());
        const auto offset = slots_[handle.slot_].offset;
        if (offset >= Bytes || base > std::numeric_limits<std::uintptr_t>::max() - offset)
            return std::nullopt;
        return CompactRef::encode(base, Bytes, base + offset);
    }

    // Validated bridge for runtimes maintaining the ORIGINAL complete Handle
    // alongside a compact edge (e.g. an out-of-line generation table). Never
    // construct a fresh proof by resolving the offset: that would reintroduce
    // ABA after slot reuse. This function does not disclose host raw pointers.
    bool compact_matches(const Handle& original, CompactRef ref) const noexcept {
        if (ref.is_null()) return false;
        const auto expected = compact(original);
        return expected.has_value() && *expected == ref;
    }

    // Allocation does not call malloc or invoke any GC. It fails closed under
    // fixed slot/byte limits. Never expose a raw pointer to actor memory.
    std::optional<Handle> allocate(
            std::size_t count, std::size_t alignment = alignof(std::max_align_t)) noexcept {
        if (retired_ || count == 0 || count > Bytes || used_ > Bytes
                || alignment == 0 || (alignment & (alignment - 1))
                || alignment > alignof(std::max_align_t)
                || (free_count_ == 0 && next_slot_ >= Slots))
            return std::nullopt;

        const auto base = reinterpret_cast<std::uintptr_t>(memory_.data());
        const auto mask = static_cast<std::uintptr_t>(alignment - 1);
        auto padding_at = [&](std::size_t physical_offset)
                -> std::optional<std::size_t> {
            if (base > std::numeric_limits<std::uintptr_t>::max() - physical_offset)
                return std::nullopt;
            const auto address = base + physical_offset;
            if (address > std::numeric_limits<std::uintptr_t>::max() - mask)
                return std::nullopt;
            const auto aligned = (address + mask) & ~mask;
            return static_cast<std::size_t>(aligned - address);
        };

        std::size_t extent_start = used_;
        std::size_t payload_offset = 0;
        std::size_t extent_bytes = 0;
        std::size_t hole_index = hole_count_;
        for (std::size_t i = 0; i < hole_count_; ++i) {
            const auto padding = padding_at(holes_[i].offset);
            if (!padding || *padding > holes_[i].length
                    || count > holes_[i].length - *padding)
                continue;
            extent_start = holes_[i].offset;
            extent_bytes = *padding + count;
            payload_offset = extent_start + *padding;
            hole_index = i;
            break;
        }
        if (hole_index == hole_count_) {
            const auto padding = padding_at(used_);
            if (!padding || *padding > Bytes - used_
                    || count > Bytes - used_ - *padding)
                return std::nullopt;
            extent_bytes = *padding + count;
            payload_offset = used_ + *padding;
            used_ += extent_bytes;
        } else {
            // The allocation owns the alignment padding; the suffix remains
            // available for subsequent allocations with any valid alignment.
            Hole& hole = holes_[hole_index];
            hole.offset += extent_bytes;
            hole.length -= extent_bytes;
            if (hole.length == 0) {
                std::move(holes_.begin() + hole_index + 1,
                          holes_.begin() + hole_count_,
                          holes_.begin() + hole_index);
                --hole_count_;
            }
        }

        const std::size_t slot_id = free_count_ != 0
            ? free_slots_[--free_count_] : next_slot_++;
        auto& slot = slots_[slot_id];
        slot.rewind_to = extent_start;
        slot.offset = payload_offset;
        slot.length = count;
        slot.live = true;
        ++live_count_;
        return Handle(heap_identity_, incarnation_, slot_id, slot.generation);
    }

    bool remove(const Handle& handle) noexcept {
        if (!valid(handle)) return false;
        auto& slot = slots_[handle.slot_];
        // Refuse ABA if the slot-generation space is exhausted.
        if (slot.generation == std::numeric_limits<std::uint64_t>::max())
            return false;
        const std::size_t extent_start = slot.rewind_to;
        const std::size_t extent_end = slot.offset + slot.length;
        // Scrub alignment padding as well as the payload. No stale bytes
        // from a prior extent can be observed on later recycling.
        std::fill_n(memory_.data() + extent_start, extent_end - extent_start,
                    std::byte{0});
        slot.live = false;
        ++slot.generation;
        free_slots_[free_count_++] = handle.slot_;
        --live_count_;
        // Deterministic ownership means no references survive when the last
        // allocation is removed. Recycle the *entire* bump region at that
        // quiescent point. Slot generations are deliberately NOT reset:
        // previously issued handles remain invalid across reuse cycles.
        // Interior holes are also reusable without compacting any live value.
        if (live_count_ == 0) {
            used_ = 0;
            hole_count_ = 0;
            std::fill(memory_.begin(), memory_.end(), std::byte{0});
        } else {
            recycle_extent(extent_start, extent_end);
        }
        return true;
    }

    template<class T>
    bool write(const Handle& handle, const T& value,
               std::size_t offset = 0) noexcept {
        static_assert(std::is_trivially_copyable<T>::value,
                      "only trivially copyable native values; no hidden drops");
        if (!valid(handle)) return false;
        const auto& slot = slots_[handle.slot_];
        if (offset > slot.length || sizeof(T) > slot.length - offset)
            return false;
        std::memcpy(memory_.data() + slot.offset + offset, &value, sizeof(T));
        return true;
    }

    template<class T>
    std::optional<T> read(
            const Handle& handle, std::size_t offset = 0) const noexcept {
        static_assert(std::is_trivially_copyable<T>::value
                      && std::is_nothrow_default_constructible<T>::value,
                      "only trivially copyable values with noexcept default construction");
        if (!valid(handle)) return std::nullopt;
        const auto& slot = slots_[handle.slot_];
        if (offset > slot.length || sizeof(T) > slot.length - offset)
            return std::nullopt;
        T value{};
        std::memcpy(&value, memory_.data() + slot.offset + offset, sizeof(T));
        return value;
    }

    // Supervisor must first prove no active actor handlers, pending wakers,
    // FFI pins, in-flight operations or escaped references. This class does
    // not itself implement the global quiescence protocol.
    void retire_quiesced() noexcept {
        if (retired_) return;
        std::fill(memory_.begin(), memory_.end(), std::byte{0});
        retired_ = true;
        used_ = 0;
        live_count_ = 0;
        free_count_ = 0;
        hole_count_ = 0;
        next_slot_ = 0;
        // Permanent retirement: new actors construct a fresh unique heap.
        // Existing handles can never resolve again, even at the same address.
    }
};

} // namespace oreslang::runtime
