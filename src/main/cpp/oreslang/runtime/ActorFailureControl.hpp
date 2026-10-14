#pragma once

#include "oreslang/runtime/ActorErrorCatalog.hpp"
#include "oreslang/runtime/ExceptionPolicy.hpp"

#include <cstdint>
#include <string_view>
#include <utility>

namespace oreslang::runtime {

// Host-only CONTROL metadata until native guest exception/unwind lowering exists.
// Never accept an exception_ptr, actor heap handle, raw Error or guest diagnostic.
enum class ActorExceptionalEffect : std::uint8_t { Raise, Panic };

class ActorFailureControlNotice {
public:
    static ActorFailureControlNotice from_guest_effect(
            ActorExceptionalEffect effect,
            std::string_view actor_id,
            std::string_view supervisor_id) {
        // Generic catalog envelopes also represent process errors and permit
        // an empty identity. A terminal actor CONTROL notice must not.
        if (actor_id.empty()) {
            throw std::invalid_argument("actor CONTROL notice requires actor identity");
        }
        switch (effect) {
            case ActorExceptionalEffect::Raise:
                return ActorFailureControlNotice(effect,
                        ActorErrorEnvelope::make(ActorErrorCode::ActorSupervision,
                                "actor raised a typed Error", actor_id, supervisor_id));
            case ActorExceptionalEffect::Panic:
                return ActorFailureControlNotice(effect,
                        ActorErrorEnvelope::make(ActorErrorCode::ActorSupervision,
                                "actor panicked with a typed Error", actor_id, supervisor_id));
        }
        throw std::invalid_argument("invalid actor exceptional effect");
    }

    ActorExceptionalEffect effect() const noexcept { return effect_; }
    const ActorErrorEnvelope& envelope() const noexcept { return envelope_; }

private:
    ActorFailureControlNotice(ActorExceptionalEffect effect,
                              ActorErrorEnvelope envelope)
            : effect_(effect), envelope_(std::move(envelope)) {}

    ActorExceptionalEffect effect_;
    ActorErrorEnvelope envelope_;
};

}  // namespace oreslang::runtime
