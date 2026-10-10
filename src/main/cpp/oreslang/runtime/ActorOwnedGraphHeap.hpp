#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <type_traits>

namespace oreslang::runtime {

// An authenticated actor execution identity is supplied by the supervisor
// and scheduler. The identity is NOT a user-visible/copyable capability.
// Native caller-side checks alone do not sandbox arbitrary hostile C++.
struct ActorHeapIdentity final {
    std::uint64_t runtime;
    std::uint64_t actor;
    std::uint64_t epoch;
};
inline bool operator==(ActorHeapIdentity a, ActorHeapIdentity b) noexcept {
    return a.runtime == b.runtime && a.actor == b.actor && a.epoch == b.epoch;
}

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
        std::array<std::byte, NodeBytes> payload{};
        std::array<Handle, LinksPerNode> links{};
        std::size_t length = 0;
        std::size_t link_count = 0;
        std::uint64_t generation = 1;
        bool live = false;
    };

    inline static std::atomic<std::uint64_t> next_heap_id_{1};
    const ActorHeapIdentity owner_;
    const std::uint64_t heap_id_;
    std::array<Slot, NodeCapacity> slots_{};
    std::size_t active_nodes_ = 0;
    std::size_t active_edges_ = 0;
    bool retired_ = false;

    void require_owner(ActorHeapIdentity requester) const {
        if (!(requester == owner_))
            throw std::logic_error("actor graph heap belongs to a different execution domain");
        if (retired_) throw std::logic_error("actor graph heap has retired");
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
        for (auto& slot : slots_) {
            std::fill(slot.payload.begin(), slot.payload.end(), std::byte{0});
            slot.links.fill(Handle{0, 0, 0});
            slot.length = 0;
            slot.link_count = 0;
            slot.live = false;
            if (slot.generation < std::numeric_limits<std::uint64_t>::max())
                ++slot.generation;
        }
        active_nodes_ = 0;
        active_edges_ = 0;
        retired_ = true;
    }

public:
    explicit ActorOwnedGraphHeap(ActorHeapIdentity owner)
        : owner_(owner), heap_id_(next_heap_id_.fetch_add(1, std::memory_order_relaxed)) {
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
    std::size_t live_nodes(ActorHeapIdentity actor) const {
        require_owner(actor);
        return active_nodes_;
    }
    std::size_t live_edges(ActorHeapIdentity actor) const {
        require_owner(actor);
        return active_edges_;
    }

    std::optional<Handle> allocate(ActorHeapIdentity actor, const void* bytes,
                                   std::size_t length) {
        require_owner(actor);
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
            slot.live = true;
            ++active_nodes_;
            return Handle{heap_id_, static_cast<std::uint32_t>(i), slot.generation};
        }
        return std::nullopt;
    }

    std::size_t copy_out(ActorHeapIdentity actor, Handle id, void* destination,
                         std::size_t capacity) {
        require_owner(actor);
        Slot& slot = require_node(id);
        if (capacity < slot.length || (slot.length && destination == nullptr))
            throw std::invalid_argument("output buffer too small");
        if (slot.length) std::memcpy(destination, slot.payload.data(), slot.length);
        return slot.length;
    }

    void write(ActorHeapIdentity actor, Handle id, const void* bytes, std::size_t length) {
        require_owner(actor);
        Slot& slot = require_node(id);
        if (length > NodeBytes || (length && !bytes))
            throw std::invalid_argument("invalid actor graph payload");
        std::fill(slot.payload.begin(), slot.payload.end(), std::byte{0});
        if (length) std::memcpy(slot.payload.data(), bytes, length);
        slot.length = length;
    }

    bool link(ActorHeapIdentity actor, Handle source, Handle target) {
        require_owner(actor);
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
        require_owner(actor);
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
        require_owner(actor);
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
        std::fill(deleted.payload.begin(), deleted.payload.end(), std::byte{0});
        deleted.links.fill(Handle{0, 0, 0});
        deleted.length = 0;
        deleted.link_count = 0;
        deleted.live = false;
        ++deleted.generation;
        --active_nodes_;
    }

    /**
     * The supervisor must prove no executing/suspended handler, pending IO,
     * FFI lease, or borrowed reference can access this heap before passing
     * quiesced=true. The flag is NOT a runtime proof; compiler/scheduler
     * lifetime fencing is a separate integration requirement.
     */
    void retire(ActorHeapIdentity actor, bool quiesced) {
        require_owner(actor);
        if (!quiesced) throw std::logic_error("actor heap retirement before quiescence");
        erase_all();
    }
};

} // namespace oreslang::runtime
