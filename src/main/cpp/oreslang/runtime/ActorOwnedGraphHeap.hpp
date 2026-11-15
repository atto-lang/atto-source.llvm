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
#include <new>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace oreslang::runtime {

// Heap domains are lifetime-unique capability IDs, not a wrapping counter.
// Reserve UINT64_MAX as terminal exhaustion so no old handle can become
// valid again if the domain allocator is stressed to its numeric limit.
namespace detail {
// Shared by ALL ActorOwnedGraphHeap template specializations in this binary.
// A class-template static counter would restart at 1 for every capacity/layout
// and permit foreign graph handles to alias a different heap configuration.
inline std::atomic<std::uint64_t> next_actor_graph_heap_id{1};

inline std::uint64_t reserve_actor_graph_heap_id(
        std::atomic<std::uint64_t>& next) {
    std::uint64_t current = next.load(std::memory_order_relaxed);
    while (current != 0 && current != std::numeric_limits<std::uint64_t>::max()) {
        if (next.compare_exchange_weak(current, current + 1,
                std::memory_order_relaxed, std::memory_order_relaxed))
            return current;
    }
    throw std::overflow_error("actor graph heap ID space exhausted");
}
} // namespace detail

// Fixed-capacity, strictly owner-confined native graph heap.
//
// Node bytes are owned by the arena; graph edges hold non-owning generational
// references. Self-cycles and mutual cycles are therefore reclaimed at the
// same O(capacity) retirement cost as acyclic graphs, without tracing GC,
// reference counting, recursion, or per-node malloc.
//
// IMPORTANT: This is a C++ runtime substrate, not yet lowered from Oreslang
// source objects. It does not implement guest destructors, borrow checking,
// FFI leases, CPU isolation, concurrent handlers or automatic quiescence.
template <std::size_t NodeCapacity, std::size_t NodeBytes, std::size_t LinksPerNode>
class ActorOwnedGraphHeap final {
    static_assert(NodeCapacity > 0 && NodeCapacity <= std::numeric_limits<std::uint32_t>::max(),
                  "invalid actor graph capacity");
    static_assert(NodeBytes > 0, "node payload must have positive size");
    static_assert(LinksPerNode > 0, "node link capacity must be positive");

    // Internal graph edges never leave this heap. Since remove() purges every
    // incoming edge before a slot can be reused, adjacency needs only the
    // heap-local slot identity; public capability handles keep generation and
    // heap identity checks at the API boundary.
    struct GraphSlotTag final {};
    using LocalLinkRef = DenseIndex32<GraphSlotTag>;
    static_assert(sizeof(LocalLinkRef) == 4,
                  "actor-local graph adjacency must remain a compact 32-bit index");

public:
    struct Handle final {
        std::uint64_t heap;
        std::uint32_t slot;
        std::uint64_t generation;
    };
    friend bool operator==(Handle a, Handle b) noexcept {
        return a.heap == b.heap && a.slot == b.slot && a.generation == b.generation;
    }

private:
    struct Slot final {
        alignas(std::max_align_t) std::array<std::byte, NodeBytes> payload{};
        // Compiler-lowered drop glue. No C++ virtual tables, heap-owned
        // closures, or C++ exceptions on deterministic actor retirement.
        void (*drop)(void*) noexcept = nullptr;
        std::array<LocalLinkRef, LinksPerNode> links{};
        std::size_t length = 0;
        std::size_t link_count = 0;
        std::uint64_t generation = 1;
        bool live = false;
        // Intrusive construction-order links; NodeCapacity is the null index.
        // Reuse does not leave historical slot entries that could double-drop.
        std::size_t older = NodeCapacity;
        std::size_t newer = NodeCapacity;
    };

    const ActorHeapIdentity owner_;
    const std::uint64_t heap_id_;
    ActorHeapBorrowGate borrow_gate_;
    std::array<Slot, NodeCapacity> slots_{};
    std::size_t active_nodes_ = 0;
    std::size_t active_edges_ = 0;
    std::size_t oldest_ = NodeCapacity;
    std::size_t newest_ = NodeCapacity;
    bool retired_ = false;
    bool dropping_ = false;

    void require_owner(ActorHeapIdentity requester) const {
        if (!(requester == owner_))
            throw std::logic_error("actor graph heap belongs to a different execution domain");
        if (retired_) throw std::logic_error("actor graph heap has retired");
    }

    void require_open_access(ActorHeapIdentity requester) const {
        require_owner(requester);
        if (borrow_gate_.sealed() || dropping_)
            throw std::logic_error("actor graph heap is retiring or executing destructor glue");
    }

    // Only trusted host buffers OUTSIDE this actor's private heap object may
    // enter a byte-copy boundary. A pointer into slots, metadata or the
    // owner/borrow gate would expose private data or corrupt allocator state.
    // Valid requests do not allocate; zero-byte copies remain no-ops.
    void require_external_span(const void* buffer, std::size_t bytes) const {
        if (!bytes) return;
        if (!buffer) throw std::invalid_argument("null actor graph host buffer");
        const auto start = reinterpret_cast<std::uintptr_t>(buffer);
        const auto owned = reinterpret_cast<std::uintptr_t>(this);
        const auto max = std::numeric_limits<std::uintptr_t>::max();
        if (bytes > max - start || sizeof(*this) > max - owned ||
                (start < owned + sizeof(*this) && owned < start + bytes))
            throw std::invalid_argument("actor graph host buffer aliases private heap or wraps");
    }

    Slot& require_node(Handle id) {
        if (id.heap != heap_id_) throw std::logic_error("foreign actor graph handle");
        if (id.slot >= NodeCapacity) throw std::logic_error("invalid actor graph slot");
        Slot& slot = slots_[id.slot];
        if (!slot.live || slot.generation != id.generation)
            throw std::logic_error("stale actor graph handle");
        return slot;
    }

    static LocalLinkRef local_link(Handle id) {
        auto ref = LocalLinkRef::from_index(id.slot);
        if (!ref) throw std::overflow_error("actor graph local index exhausted");
        return *ref;
    }

    void append_construction_order(std::size_t index) noexcept {
        Slot& slot = slots_[index];
        slot.older = newest_;
        slot.newer = NodeCapacity;
        if (newest_ == NodeCapacity) oldest_ = index;
        else slots_[newest_].newer = index;
        newest_ = index;
    }

    void detach_construction_order(std::size_t index) noexcept {
        Slot& slot = slots_[index];
        if (slot.older == NodeCapacity) oldest_ = slot.newer;
        else slots_[slot.older].newer = slot.newer;
        if (slot.newer == NodeCapacity) newest_ = slot.older;
        else slots_[slot.newer].older = slot.older;
        slot.older = NodeCapacity;
        slot.newer = NodeCapacity;
    }

    void erase_all() noexcept {
        // Revoke every public handle BEFORE any destructor can observe it.
        // Drop in reverse *successful construction* order, not physical
        // slot order: free-slot reuse must never reorder or double-drop owners.
        // The intrusive list requires no allocations and preserves the
        // O(NodeCapacity) actor retirement bound, including graph cycles.
        retired_ = true;
        dropping_ = true;
        for (std::size_t index = newest_; index != NodeCapacity;) {
            Slot& slot = slots_[index];
            const std::size_t previous = slot.older;
            auto* drop = slot.drop;
            slot.drop = nullptr;
            const bool was_live = slot.live;
            slot.live = false;
            if (was_live && drop) drop(slot.payload.data());
            index = previous;
        }
        oldest_ = newest_ = NodeCapacity;
        for (auto& slot : slots_) {
            slot.drop = nullptr;
            slot.live = false;
            std::fill(slot.payload.begin(), slot.payload.end(), std::byte{0});
            slot.links.fill(LocalLinkRef{});
            slot.length = 0;
            slot.link_count = 0;
            slot.older = slot.newer = NodeCapacity;
            if (slot.generation < std::numeric_limits<std::uint64_t>::max())
                ++slot.generation;
        }
        active_nodes_ = 0;
        active_edges_ = 0;
        dropping_ = false;
    }

public:
    explicit ActorOwnedGraphHeap(ActorHeapIdentity owner)
        : owner_(owner), heap_id_(detail::reserve_actor_graph_heap_id(
              detail::next_actor_graph_heap_id)),
          borrow_gate_(owner) {
        if (!owner_.runtime || !owner_.actor || !owner_.epoch || !heap_id_)
            throw std::invalid_argument("invalid actor graph heap identity");
    }
    ActorOwnedGraphHeap(const ActorOwnedGraphHeap&) = delete;
    ActorOwnedGraphHeap& operator=(const ActorOwnedGraphHeap&) = delete;
    ActorOwnedGraphHeap(ActorOwnedGraphHeap&&) = delete;
    ActorOwnedGraphHeap& operator=(ActorOwnedGraphHeap&&) = delete;
    ~ActorOwnedGraphHeap() noexcept {
        // Destructor entry used to bypass the authenticated runtime retire()
        // gate. In particular, a live Lease stores a pointer to borrow_gate_,
        // which becomes dangling if this heap's owner destroys it early.
        //
        // There is no safe way to quarantine a stack-allocated C++ object
        // after its destructor runs. Fail-stop rather than run drop glue,
        // zero/free native payloads or invalidate an outstanding gate.
        if (!borrow_gate_.seal_and_quiesced(owner_)) std::terminate();
        erase_all();
    }

    std::uint64_t heap_id() const noexcept { return heap_id_; }
    static constexpr std::size_t local_link_storage_bytes() noexcept {
        return sizeof(LocalLinkRef);
    }
    ActorHeapIdentity owner() const noexcept { return owner_; }
    bool retired() const noexcept { return retired_; }

    /** Supervisor/FFI must hold a lease before any reference survives a turn. */
    std::optional<ActorHeapBorrowGate::Lease> borrow(ActorHeapIdentity actor) {
        require_owner(actor);
        return borrow_gate_.acquire(actor);
    }
    std::size_t live_nodes(ActorHeapIdentity actor) const {
        require_open_access(actor);
        return active_nodes_;
    }
    std::size_t live_edges(ActorHeapIdentity actor) const {
        require_open_access(actor);
        return active_edges_;
    }

    std::optional<Handle> allocate(ActorHeapIdentity actor, const void* bytes,
                                   std::size_t length) {
        require_open_access(actor);
        if (length > NodeBytes || (length && bytes == nullptr))
            throw std::invalid_argument("invalid actor graph payload");
        require_external_span(bytes, length);
        for (std::size_t i = 0; i < NodeCapacity; ++i) {
            Slot& slot = slots_[i];
            if (slot.live || slot.generation == std::numeric_limits<std::uint64_t>::max())
                continue;
            // Initialize only after complete argument validation.
            std::fill(slot.payload.begin(), slot.payload.end(), std::byte{0});
            if (length) std::memcpy(slot.payload.data(), bytes, length);
            slot.length = length;
            slot.link_count = 0;
            slot.drop = nullptr;
            slot.live = true;
            append_construction_order(i);
            ++active_nodes_;
            return Handle{heap_id_, static_cast<std::uint32_t>(i), slot.generation};
        }
        return std::nullopt;
    }

    /**
     * Explicit native object allocation for future compiler lowering.
     * Unlike raw byte slots, these slots MUST run their registered destructor
     * exactly once before storage can be recycled or the actor heap retires.
     */
    template<class T, class... Args>
    std::optional<Handle> emplace_owned(ActorHeapIdentity actor, Args&&... args) {
        static_assert(sizeof(T) <= NodeBytes, "actor object exceeds slot capacity");
        static_assert(alignof(T) <= alignof(std::max_align_t),
                      "overaligned actor object requires a different arena");
        static_assert(std::is_nothrow_constructible<T, Args...>::value,
                      "actor heap constructors must not throw");
        static_assert(std::is_nothrow_destructible<T>::value,
                      "actor heap drop glue must not throw");
        require_open_access(actor);
        for (std::size_t i = 0; i < NodeCapacity; ++i) {
            Slot& slot = slots_[i];
            if (slot.live || slot.generation == std::numeric_limits<std::uint64_t>::max())
                continue;
            std::fill(slot.payload.begin(), slot.payload.end(), std::byte{0});
            void* memory = slot.payload.data();
            ::new (memory) T(std::forward<Args>(args)...);
            slot.length = sizeof(T);
            slot.link_count = 0;
            slot.drop = [](void* address) noexcept {
                static_cast<T*>(address)->~T();
            };
            slot.live = true;
            append_construction_order(i);
            ++active_nodes_;
            return Handle{heap_id_, static_cast<std::uint32_t>(i), slot.generation};
        }
        return std::nullopt;
    }

    std::size_t copy_out(ActorHeapIdentity actor, Handle id, void* destination,
                         std::size_t capacity) {
        require_open_access(actor);
        Slot& slot = require_node(id);
        if (slot.drop) throw std::logic_error("owned objects are not raw byte payloads");
        if (capacity < slot.length || (slot.length && destination == nullptr))
            throw std::invalid_argument("output buffer too small");
        require_external_span(destination, slot.length);
        if (slot.length) std::memcpy(destination, slot.payload.data(), slot.length);
        return slot.length;
    }

    void write(ActorHeapIdentity actor, Handle id, const void* bytes, std::size_t length) {
        require_open_access(actor);
        Slot& slot = require_node(id);
        if (slot.drop) throw std::logic_error("owned objects cannot be overwritten as bytes");
        if (length > NodeBytes || (length && !bytes))
            throw std::invalid_argument("invalid actor graph payload");
        require_external_span(bytes, length);
        std::fill(slot.payload.begin(), slot.payload.end(), std::byte{0});
        if (length) std::memcpy(slot.payload.data(), bytes, length);
        slot.length = length;
    }

    bool link(ActorHeapIdentity actor, Handle source, Handle target) {
        require_open_access(actor);
        Slot& from = require_node(source);
        (void)require_node(target);
        const LocalLinkRef local_target = local_link(target);
        for (std::size_t i = 0; i < from.link_count; ++i) {
            if (from.links[i] == local_target) return false;
        }
        if (from.link_count == LinksPerNode) return false;
        from.links[from.link_count++] = local_target;
        ++active_edges_;
        return true;
    }

    bool is_linked(ActorHeapIdentity actor, Handle source, Handle target) {
        require_open_access(actor);
        Slot& from = require_node(source);
        // Querying a stale target is a capability error, not a valid
        // negative adjacency result: reject stale generations and slots.
        (void)require_node(target);
        const LocalLinkRef local_target = local_link(target);
        for (std::size_t i = 0; i < from.link_count; ++i) {
            if (from.links[i] == local_target) return true;
        }
        return false;
    }

    // Invalidating a node also purges incoming non-owning edges, to avoid
    // dangling links consuming a bounded edge budget across slot reuse.
    void remove(ActorHeapIdentity actor, Handle id) {
        require_open_access(actor);
        Slot& deleted = require_node(id);
        if (deleted.generation == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("actor graph node generation exhausted");
        const LocalLinkRef deleted_ref = local_link(id);
        for (auto& slot : slots_) {
            if (!slot.live) continue;
            std::size_t keep = 0;
            const std::size_t before = slot.link_count;
            for (std::size_t i = 0; i < before; ++i) {
                if (!(slot.links[i] == deleted_ref)) slot.links[keep++] = slot.links[i];
            }
            slot.link_count = keep;
            active_edges_ -= before - keep;
        }
        active_edges_ -= deleted.link_count;
        auto* drop = deleted.drop;
        detach_construction_order(id.slot);
        deleted.drop = nullptr;
        deleted.live = false;
        ++deleted.generation;
        --active_nodes_;
        dropping_ = true;
        if (drop) drop(deleted.payload.data());
        dropping_ = false;
        std::fill(deleted.payload.begin(), deleted.payload.end(), std::byte{0});
        deleted.links.fill(LocalLinkRef{});
        deleted.length = 0;
        deleted.link_count = 0;
    }

    /**
     * A validated CarrierRetirementProof can be minted ONLY after the native
     * carrier and broker joined and all mailbox producers drained. Moreover,
     * every external borrow/FFI/continuation lease must have been released.
     * The monotonic gate seals admissions before inspecting borrow count:
     * no new borrow may slip between the check and memory reclamation.
     */
    void retire(ActorHeapIdentity actor, const CarrierRetirementProof& proof) {
        require_owner(actor);
        if (proof.owner() != owner_)
            throw std::logic_error("retirement proof belongs to another actor generation");
        if (!borrow_gate_.seal_and_quiesced(actor))
            throw std::logic_error("actor heap still has live borrow/FFI leases");
        erase_all();
    }
};

} // namespace oreslang::runtime
