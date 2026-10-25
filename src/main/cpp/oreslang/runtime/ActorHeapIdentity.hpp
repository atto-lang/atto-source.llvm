#pragma once
#include "oreslang/runtime/NativeExecutionContext.h"
#include <cstdint>

namespace oreslang::runtime {
// Authenticated by the supervisor at native actor admission, not a capability
// exposed to untrusted C++ or arbitrary guest source.
struct ActorHeapIdentity final {
    std::uint64_t runtime;
    std::uint64_t actor;
    std::uint64_t epoch;
};
inline bool operator==(ActorHeapIdentity a, ActorHeapIdentity b) noexcept {
    return a.runtime == b.runtime && a.actor == b.actor && a.epoch == b.epoch;
}
inline bool operator!=(ActorHeapIdentity a, ActorHeapIdentity b) noexcept {
    return !(a == b);
}

/*
 * Actor heap identity is an allocation-domain capability. actor==0 is valid;
 * runtime and epoch provide the non-zero authority/generation fences.
 * Logical ExecutionId is intentionally not part of the conversion.
 */
inline OresNativeAllocationDomainRef actor_allocation_domain(
        ActorHeapIdentity identity) noexcept {
    return OresNativeAllocationDomainRef{
        ORES_ALLOCATION_DOMAIN_ACTOR,
        identity.runtime,
        identity.actor,
        identity.epoch
    };
}

inline bool same_actor_allocation_domain(
        OresNativeAllocationDomainRef capability,
        ActorHeapIdentity identity) noexcept {
    const auto expected = actor_allocation_domain(identity);
    return capability.kind == expected.kind
        && capability.domain == expected.domain
        && capability.owner == expected.owner
        && capability.generation == expected.generation;
}
} // namespace oreslang::runtime
