#pragma once

#include "oreslang/runtime/ActorHeapRetirementProof.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace oreslang::runtime {

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
        std::array<Handle, LinksPerNode> links{};
        std::size_t length = 0;
        std::size_t link_count = 0;
        std::uint64_t generation = 1;
        bool live = false;
    };

    inline static std::atomic<std::uint64_t> next_heap_id_{1};
    const ActorHeapIdentity owner_;
    const std::uint64_t heap_id_;
    ActorHeapBorrowGate borrow_gate_;
    std::array<Slot, NodeCapacity> slots_{};
    std::size_t active_nodes_ = 0;
    std::size_t active_edges_ = 0;
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

    Slot& require_node(Handle id) {
        if (id.heap != heap_id_) throw std::logic_error("foreign actor graph handle");
        if (id.slot >= NodeCapacity) throw std::logic_error("invalid actor graph slot");
        Slot& slot = slots_[id.slot];
        if (!slot.live || slot.generation != id.generation)
            throw std::logic_error("stale actor graph handle");
        return slot;
    }

    void erase_all() noexcept {
        // Revoke every public handle BEFORE any destructor can observe it.
        // Destructor glue must not reenter this arena or follow weak links.
        retired_ = true;
        dropping_ = true;
        for (auto& slot : slots_) {
            auto* drop = slot.drop;
            slot.drop = nullptr;
            const bool was_live = slot.live;
            slot.live = false;
            if (was_live && drop) drop(slot.payload.data());
            std::fill(slot.payload.begin(), slot.payload.end(), std::byte{0});
            slot.links.fill(Handle{0, 0, 0});
            slot.length = 0;
            slot.link_count = 0;
            if (slot.generation < std::numeric_limits<std::uint64_t>::max())
                ++slot.generation;
        }
        active_nodes_ = 0;
        active_edges_ = 0;
        dropping_ = false;
    }

public:
    explicit ActorOwnedGraphHeap(ActorHeapIdentity owner)
        : owner_(owner), heap_id_(next_heap_id_.fetch_add(1, std::memory_order_relaxed)),
          borrow_gate_(owner) {
        if (!owner_.runtime || !owner_.actor || !owner_.epoch || !heap_id_)
            throw std::invalid_argument("invalid actor graph heap identity");
    }
    ActorOwnedGraphHeap(const ActorOwnedGraphHeap&) = delete;
    ActorOwnedGraphHeap& operator=(const ActorOwnedGraphHeap&) = delete;
    ActorOwnedGraphHeap(ActorOwnedGraphHeap&&) = delete;
    ActorOwnedGraphHeap& operator=(ActorOwnedGraphHeap&&) = delete;
    ~ActorOwnedGraphHeap() { erase_all(); }

    std::uint64_t heap_id() const noexcept { return heap_id_; }
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
        if (slot.length) std::memcpy(destination, slot.payload.data(), slot.length);
        return slot.length;
    }

    void write(ActorHeapIdentity actor, Handle id, const void* bytes, std::size_t length) {
        require_open_access(actor);
        Slot& slot = require_node(id);
        if (slot.drop) throw std::logic_error("owned objects cannot be overwritten as bytes");
        if (length > NodeBytes || (length && !bytes))
            throw std::invalid_argument("invalid actor graph payload");
        std::fill(slot.payload.begin(), slot.payload.end(), std::byte{0});
        if (length) std::memcpy(slot.payload.data(), bytes, length);
        slot.length = length;
    }

    bool link(ActorHeapIdentity actor, Handle source, Handle target) {
        require_open_access(actor);
        Slot& from = require_node(source);
        (void)require_node(target);
        for (std::size_t i = 0; i < from.link_count; ++i) {
            if (from.links[i] == target) return false;
        }
        if (from.link_count == LinksPerNode) return false;
        from.links[from.link_count++] = target;
        ++active_edges_;
        return true;
    }

    bool is_linked(ActorHeapIdentity actor, Handle source, Handle target) {
        require_open_access(actor);
        Slot& from = require_node(source);
        if (target.heap != heap_id_) throw std::logic_error("foreign actor graph handle");
        for (std::size_t i = 0; i < from.link_count; ++i) {
            if (from.links[i] == target) return true;
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
        for (auto& slot : slots_) {
            if (!slot.live) continue;
            std::size_t keep = 0;
            const std::size_t before = slot.link_count;
            for (std::size_t i = 0; i < before; ++i) {
                if (!(slot.links[i] == id)) slot.links[keep++] = slot.links[i];
            }
            slot.link_count = keep;
            active_edges_ -= before - keep;
        }
        active_edges_ -= deleted.link_count;
        auto* drop = deleted.drop;
        deleted.drop = nullptr;
        deleted.live = false;
        ++deleted.generation;
        --active_nodes_;
        dropping_ = true;
        if (drop) drop(deleted.payload.data());
        dropping_ = false;
        std::fill(deleted.payload.begin(), deleted.payload.end(), std::byte{0});
        deleted.links.fill(Handle{0, 0, 0});
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
