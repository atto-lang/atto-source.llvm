#pragma once

#include "oreslang/backend/ManagedRefMir.hpp"

#include <cstdint>
#include <string_view>

namespace oreslang {

/*
 * Sole production source authority for ActorRefOwnershipEvidence.
 *
 * This API intentionally accepts source text, not caller-authored Program/HIR,
 * ownership graphs, MIR claims or booleans. It reparses and revalidates the
 * source, derives the managed-ref use and loan mode from that program, builds
 * the ownership CFG internally, and only then mints backend evidence.
 *
 * The first vertical slice supports actor-private Ref<int> declared in the
 * single sealed state schema and one terminal ref_read/ref_write operation.
 */
class SourceOwnershipChecker final {
public:
    static backend::ActorRefOwnershipEvidence issue_actor_managed_ref_evidence(
        std::string_view source,
        std::string_view actor_name,
        std::string_view ref_field,
        std::uint64_t actor_region_capacity_bytes,
        bool native64_target);
};

} // namespace oreslang
