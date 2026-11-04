#pragma once

// HOST ONLY: a native scheduler-authenticated request dispatch adapter.
// Possession of a RequestContextLocalV1::Ptr or an ActorRef is NOT authority.
// Only native execution claims on the actual owner pthread, original logical
// request-mail provenance, and the live request incarnation may access locals.
// AOT compiler lowering and a scheduler-minted opaque guest receipt remain
// required before this bridge may authorize .atto guest execution.

#include "oreslang/runtime/NativeExecutionAuthority.h"
#include "oreslang/runtime/RequestContextLocalV1.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

namespace oreslang::runtime {

struct NativeRequestMailProofV1 {
    OresExecutionClaim actor_turn{};
    std::uint64_t original_mail_sequence = 0;
};

// Native-only adapter; the host must create this once at verified ingress,
// passing the original delivered mail sequence, not a callback/guest-provided
// string. The proof itself is not a bearer capability: copied claims are
// rejected on foreign threads by NativeExecutionAuthority.
class NativeRequestMailGateV1 final {
public:
    NativeRequestMailGateV1(
            OresNativeExecutionAuthority& authority,
            RequestContextLocalV1::Ptr request,
            OresNativeActorRef actor,
            std::uint64_t admitted_mail_sequence)
        : authority_(&authority), request_(std::move(request)),
          actor_(actor), admitted_mail_sequence_(admitted_mail_sequence) {
        if (!request_ || !request_->identity().valid()
                || !request_->belongs_to(actor_)
                || actor_.generation == 0) {
            throw std::invalid_argument("invalid native request-mail owner");
        }
    }

    NativeRequestMailGateV1(const NativeRequestMailGateV1&) = delete;
    NativeRequestMailGateV1& operator=(const NativeRequestMailGateV1&) = delete;

    bool authorized(const NativeRequestMailProofV1& current) const {
        if (current.original_mail_sequence != admitted_mail_sequence_
                || current.actor_turn.carrier == 0
                || current.actor_turn.generation != actor_.generation)
            return false;
        // Supervisor STOP revokes request access even while an already
        // admitted cleanup turn still owns its physical native carrier.
        // TaskCancellation remains the independent physical lifetime fence.
        return ores_execution_owns(authority_, current.actor_turn)
            && ores_execution_phase(authority_) == ORES_EXEC_RUNNING
            && request_->belongs_to(actor_) && request_->active();
    }

    template <typename T>
    bool set(const NativeRequestMailProofV1& current,
             const RequestLocalKeyV1<T>& key, T value) {
        if (!authorized(current)) return false;
        return request_->set(key, std::move(value));
    }

    template <typename T>
    std::optional<T> get_copy(const NativeRequestMailProofV1& current,
                              const RequestLocalKeyV1<T>& key) const {
        if (!authorized(current)) return std::nullopt;
        return request_->get_copy(key);
    }

    template <typename T>
    bool remove(const NativeRequestMailProofV1& current,
                const RequestLocalKeyV1<T>& key) {
        if (!authorized(current)) return false;
        return request_->remove(key);
    }

private:
    OresNativeExecutionAuthority* const authority_;
    const RequestContextLocalV1::Ptr request_;
    const OresNativeActorRef actor_;
    const std::uint64_t admitted_mail_sequence_;
};

} // namespace oreslang::runtime
