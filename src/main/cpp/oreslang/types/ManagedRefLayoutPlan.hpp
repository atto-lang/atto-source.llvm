#pragma once

#include "oreslang/runtime/ManagedRef.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace oreslang::analysis {

// Compiler-side physical field planning for typed logical references.
// The native parser currently has only i64 class fields and opaque array
// handles. It must NOT reinterpret those as ReferenceDomain::ActorPrivate.
// This function can be invoked only after a future typed HIR/ownership pass
// establishes the evidence below. It does not mint any runtime capability.
enum class ReferenceDomain : std::uint8_t {
    ActorPrivate, CrossActor, SharedImmutable, ForeignNative, DenseRow
};
enum class PhysicalRefStorage : std::uint8_t {
    ActorOffset32, ActorAddress64, ActorCapability, SharedHandle,
    NativePointer, DenseIndex32
};
struct RefLayoutEvidence final {
    ReferenceDomain domain = ReferenceDomain::ActorPrivate;
    std::uint32_t target_pointer_bits = 64;
    std::uint64_t maximum_arena_span = 0;
    std::uint64_t maximum_dense_rows = 0;
    std::uint32_t minimum_alignment = 8;
    bool actor_heap_bound = false;
    bool owner_proven = false;
    // Static borrow proof or original-generation runtime ledger, not a
    // generation recomputed from a raw compressed offset.
    bool lifetime_proven = false;
    bool force_native64 = false;
    bool share_authorized = false;
    bool dense_index_proven = false;
};
struct RefLayoutPlan final {
    PhysicalRefStorage storage;
    // Zero means an intentionally opaque handle/capability ABI; width is
    // deliberately NOT guessed from a pointer or integer type.
    std::uint32_t field_bytes;
    std::uint32_t alignment;
    bool needs_actor_base_at_dereference;
};

inline std::optional<RefLayoutPlan> plan_managed_reference(
        const RefLayoutEvidence& evidence) noexcept {
    const bool native32 = evidence.target_pointer_bits == 32;
    const bool native64 = evidence.target_pointer_bits == 64;
    if (!native32 && !native64) return std::nullopt;
    switch (evidence.domain) {
        case ReferenceDomain::ForeignNative:
            return RefLayoutPlan{PhysicalRefStorage::NativePointer,
                                 evidence.target_pointer_bits / 8,
                                 evidence.target_pointer_bits / 8, false};
        case ReferenceDomain::CrossActor:
            return RefLayoutPlan{PhysicalRefStorage::ActorCapability, 0, 0, false};
        case ReferenceDomain::SharedImmutable:
            if (!evidence.share_authorized) return std::nullopt;
            return RefLayoutPlan{PhysicalRefStorage::SharedHandle, 0, 0, false};
        case ReferenceDomain::DenseRow:
            if (!evidence.actor_heap_bound || !evidence.owner_proven
                    || !evidence.lifetime_proven || !evidence.dense_index_proven
                    || evidence.maximum_dense_rows == 0
                    || evidence.maximum_dense_rows >
                       static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) - 1)
                return std::nullopt;
            return RefLayoutPlan{PhysicalRefStorage::DenseIndex32, 4, 4, false};
        case ReferenceDomain::ActorPrivate:
            if (!evidence.actor_heap_bound || !evidence.owner_proven
                    || !evidence.lifetime_proven || !evidence.maximum_arena_span
                    || !native64 || evidence.minimum_alignment < 8
                    || (evidence.minimum_alignment & (evidence.minimum_alignment - 1)))
                return std::nullopt;
            if (!evidence.force_native64
                    && evidence.maximum_arena_span <=
                       runtime::CompressedActorRef::max_addressable_span) {
                return RefLayoutPlan{PhysicalRefStorage::ActorOffset32, 4, 8, true};
            }
            // Only a separately proven actor-owned native64 reference may
            // bypass the compressed arena span/alignment limitation.
            return RefLayoutPlan{PhysicalRefStorage::ActorAddress64, 8, 8, false};
    }
    return std::nullopt;
}

} // namespace oreslang::analysis
