#pragma once

#include "oreslang/runtime/ActorOwnedHeap.hpp"
#include "oreslang/runtime/ManagedRef.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>

namespace oreslang::runtime {

// First native substrate for compiler-lowered actor-local Ref<T> fields.
//
// Object *storage* contains exactly one 32-bit compressed location per edge.
// An out-of-line ledger retains the ORIGINAL full heap/generation proof for
// each edge until source-level borrow/ownership analysis can eliminate it.
// A byte offset alone is NEVER sufficient to authorize dereference: offset
// recycling can otherwise resurrect an old reference (ABA). The ledger is
// host-owned and must not be serialized or exposed to untrusted guest code.
//
// This is a bounded, strictly single-carrier experimental object store, NOT
// general Oreslang source class lowering, a sandbox, or a replacement for
// cross-actor ActorRef<T>. The scheduler must enforce owner-turn admission and
// quiescence before destroying this store.
template<std::size_t Bytes, std::size_t Nodes, std::size_t LinksPerNode>
class ActorCompactFieldStore final {
    static_assert(Nodes > 0 && Nodes <= UINT32_MAX, "invalid node capacity");
    static_assert(LinksPerNode > 0, "invalid link capacity");

    using Heap = ActorOwnedHeap<Bytes, Nodes>;
    using Proof = typename Heap::Handle;

public:
    // Oreslang guest object layout: the payload is just a stand-in for the
    // compiler's class fields; the link array is genuinely stored as i32s.
    struct Object final {
        std::int64_t payload = 0;
        std::array<CompressedActorRef, LinksPerNode> links{};
    };
    static_assert(sizeof(CompressedActorRef) == 4, "compact fields must be i32");
    static_assert(std::is_trivially_copyable<Object>::value, "native POD object required");
    static_assert(Bytes >= sizeof(Object), "object exceeds actor heap capacity");

    class Node final {
        friend class ActorCompactFieldStore;
        Proof proof_{};
        std::uint32_t slot_ = UINT32_MAX;
        explicit Node(Proof proof, std::uint32_t slot) noexcept
            : proof_(proof), slot_(slot) {}
    public:
        Node() = default; // unusable until created by the store
    };

    enum class LinkState : std::uint8_t {
        InvalidSource, Empty, Live, Stale
    };
    struct LinkResult final {
        LinkState state = LinkState::InvalidSource;
        std::optional<Node> target{};
    };

private:
    Heap heap_;
    // The ledger is *not* embedded in Object and is not a guest-visible
    // pointer. One entry per source-object/field records the original target
    // node generation, never a freshly reconstructed proof from an offset.
    std::array<std::optional<Proof>, Nodes> owners_{};
    std::array<std::array<std::optional<Node>, LinksPerNode>, Nodes> proofs_{};

    bool live(Node n) const noexcept {
        return n.slot_ < Nodes && owners_[n.slot_].has_value()
            && *owners_[n.slot_] == n.proof_
            && heap_.compact(n.proof_).has_value();
    }

public:
    explicit ActorCompactFieldStore(std::uint64_t actor_id) : heap_(actor_id) {}
    ActorCompactFieldStore(const ActorCompactFieldStore&) = delete;
    ActorCompactFieldStore& operator=(const ActorCompactFieldStore&) = delete;

    static constexpr std::size_t link_bytes = sizeof(CompressedActorRef);
    static constexpr std::size_t object_bytes = sizeof(Object);
    std::uint64_t actor_id() const noexcept { return heap_.actor_id(); }
    bool retired() const noexcept { return heap_.retired(); }

    std::optional<Node> create(std::int64_t payload) noexcept {
        if (heap_.retired()) return std::nullopt;
        std::size_t slot = 0;
        while (slot < Nodes && owners_[slot]) ++slot;
        if (slot == Nodes) return std::nullopt;
        auto proof = heap_.allocate(sizeof(Object), alignof(Object));
        if (!proof) return std::nullopt;
        Object object{};
        object.payload = payload;
        if (!heap_.write(*proof, object)) {
            (void)heap_.remove(*proof);
            return std::nullopt;
        }
        owners_[slot] = *proof;
        proofs_[slot].fill(std::nullopt);
        return Node(*proof, static_cast<std::uint32_t>(slot));
    }

    std::optional<std::int64_t> read_value(Node node) const noexcept {
        if (!live(node)) return std::nullopt;
        auto object = heap_.template read<Object>(node.proof_);
        if (!object) return std::nullopt;
        return object->payload;
    }

    bool write_value(Node node, std::int64_t value) noexcept {
        if (!live(node)) return false;
        auto object = heap_.template read<Object>(node.proof_);
        if (!object) return false;
        object->payload = value;
        return heap_.write(node.proof_, *object);
    }

    bool link(Node from, std::size_t field, Node to) noexcept {
        if (field >= LinksPerNode || !live(from) || !live(to)) return false;
        const auto ref = heap_.compact(to.proof_);
        auto object = heap_.template read<Object>(from.proof_);
        if (!ref || ref->is_null() || !object) return false;
        object->links[field] = *ref;
        // Publish the physical field before its original-generation proof.
        // The trusted actor turn must exclude concurrent readers/handlers.
        if (!heap_.write(from.proof_, *object)) return false;
        proofs_[from.slot_][field] = to;
        return true;
    }

    bool unlink(Node from, std::size_t field) noexcept {
        if (field >= LinksPerNode || !live(from)) return false;
        auto object = heap_.template read<Object>(from.proof_);
        if (!object) return false;
        object->links[field] = CompressedActorRef{};
        if (!heap_.write(from.proof_, *object)) return false;
        proofs_[from.slot_][field].reset();
        return true;
    }

    LinkResult follow(Node from, std::size_t field) const noexcept {
        if (field >= LinksPerNode || !live(from))
            return {LinkState::InvalidSource, std::nullopt};
        auto object = heap_.template read<Object>(from.proof_);
        if (!object) return {LinkState::InvalidSource, std::nullopt};
        const auto ref = object->links[field];
        const auto& original = proofs_[from.slot_][field];
        if (ref.is_null() && !original)
            return {LinkState::Empty, std::nullopt};
        if (ref.is_null() || !original || !live(*original)
                || !heap_.compact_matches(original->proof_, ref))
            return {LinkState::Stale, std::nullopt};
        return {LinkState::Live, *original};
    }

    // Allows tests and generated runtime instrumentation to confirm that
    // object fields are physically 4 bytes. Never returns writable memory.
    std::optional<Object> snapshot(Node node) const noexcept {
        if (!live(node)) return std::nullopt;
        return heap_.template read<Object>(node.proof_);
    }

    bool remove(Node node) noexcept {
        if (!live(node) || !heap_.remove(node.proof_)) return false;
        // Do not rebind other objects' inbound edges to a recycled location.
        // Their ledger retains the OLD proof and follow() reports Stale.
        owners_[node.slot_].reset();
        proofs_[node.slot_].fill(std::nullopt);
        return true;
    }

    void retire_quiesced() noexcept {
        heap_.retire_quiesced();
        owners_.fill(std::nullopt);
        for (auto& row : proofs_) row.fill(std::nullopt);
    }
};

} // namespace oreslang::runtime
