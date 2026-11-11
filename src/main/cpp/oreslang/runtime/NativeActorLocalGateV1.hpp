#pragma once

// NATIVE HOST-ONLY authenticated actor dispatch adapter for scoped locals.
// The native compiler must still lower real .atto source and prove AOT ABI.
// The actor runtime, not user source, must construct this adapter from its
// scheduler-owned execution authority and actor incarnation. It does not
// turn arbitrary caller-provided identity metadata into guest privilege.

#include "oreslang/runtime/NativeExecutionAuthority.h"
#include "oreslang/runtime/ScopedLocalStorageV1.hpp"

#include <optional>
#include <utility>

namespace oreslang::runtime {

class NativeActorLocalGateV1 final {
public:
    NativeActorLocalGateV1(OresNativeExecutionAuthority& authority,
                           ActorLocalStoreV1& storage,
                           ScopedLocalIdentityV1 owner)
        : authority_(&authority), storage_(&storage), owner_(owner) {
        if (!owner_.valid()) throw std::invalid_argument("invalid actor local gate owner");
    }

    NativeActorLocalGateV1(const NativeActorLocalGateV1&) = delete;
    NativeActorLocalGateV1& operator=(const NativeActorLocalGateV1&) = delete;

    // Physical-thread ownership, exact claim generation/turn sequence and
    // logical actor store incarnation must ALL match. A copied claim on a
    // producer callback thread is never enough.
    bool authorized(const OresExecutionClaim& claim) const {
        return claim.carrier != 0 && claim.generation == owner_.generation
            && ores_execution_owns(authority_, claim)
            && storage_->active_for(owner_);
    }

    template <class T>
    bool set(const OresExecutionClaim& claim,
             const ActorLocalKeyV1<T>& key, T value) {
        if (!authorized(claim)) return false;
        return storage_->set(key, owner_, std::move(value));
    }

    template <class T>
    std::optional<T> get_copy(const OresExecutionClaim& claim,
                              const ActorLocalKeyV1<T>& key) const {
        if (!authorized(claim)) return std::nullopt;
        return storage_->get_copy(key, owner_);
    }

    template <class T>
    bool remove(const OresExecutionClaim& claim,
                const ActorLocalKeyV1<T>& key) {
        if (!authorized(claim)) return false;
        return storage_->remove(key, owner_);
    }

private:
    OresNativeExecutionAuthority* const authority_;
    ActorLocalStoreV1* const storage_;
    const ScopedLocalIdentityV1 owner_;
};

} // namespace oreslang::runtime
