#pragma once

// HOST-ONLY native request origin adapter: source guest code cannot supply
// current logical mail provenance. The scheduler creates the gate after
// admitting ingress and provides a trusted, nonallocating current-mail reader.
// A copied ActorRef, token string, or actor execution claim is not enough.
// Production codegen, dispatch callback wiring, and guest AOT lowering remain
// mandatory release gates; this header alone is not a tenant sandbox.

#include "oreslang/runtime/NativeExecutionAuthority.h"
#include "oreslang/runtime/RequestContextLocalV1.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

namespace oreslang::runtime {

// Only a trusted native actor dispatcher may supply this function pointer
// and the pinned context it reads. For ordinary user mail, it returns the
// exact source-mail sequence. For a scheduler-approved SourceTask resume, it
// returns that immutable original sequence. For a generic Future completion,
// callback-only thread, detached Task, STOP cleanup, or unknown carrier it
// MUST return std::nullopt. It must not inspect arbitrary guest arguments.
using TrustedRequestMailReaderV1 =
    std::optional<std::uint64_t> (*)(void*) noexcept;

class NativeRequestMailGateV1 final {
public:
    NativeRequestMailGateV1(
            OresNativeExecutionAuthority& authority,
            RequestContextLocalV1::Ptr request,
            OresNativeActorRef actor,
            std::uint64_t admitted_mail_sequence,
            TrustedRequestMailReaderV1 current_mail,
            void* pinned_scheduler_context)
        : authority_(&authority), request_(std::move(request)),
          actor_(actor), admitted_mail_sequence_(admitted_mail_sequence),
          current_mail_(current_mail),
          scheduler_context_(pinned_scheduler_context) {
        if (!request_ || !request_->identity().valid()
                || !request_->belongs_to(actor_)
                || actor_.generation == 0 || !current_mail_
                || !scheduler_context_) {
            throw std::invalid_argument("invalid native request-mail owner");
        }
    }

    NativeRequestMailGateV1(const NativeRequestMailGateV1&) = delete;
    NativeRequestMailGateV1& operator=(const NativeRequestMailGateV1&) = delete;

    bool authorized(const OresExecutionClaim& current) const {
        if (!current.carrier || current.generation != actor_.generation)
            return false;
        // One mutex acquisition validates real physical owner and RUNNING
        // phase. STOPPING cleanup may still hold the lease, but never a live
        // RequestLocal grant. Native cancellation/physical pin is separate.
        if (!ores_execution_owns_running(authority_, current)) return false;
        const auto admitted = current_mail_(scheduler_context_);
        return admitted.has_value()
            && *admitted == admitted_mail_sequence_
            && request_->belongs_to(actor_) && request_->active();
    }

    template <typename T>
    bool set(const OresExecutionClaim& current,
             const RequestLocalKeyV1<T>& key, T value) {
        if (!authorized(current)) return false;
        return request_->set(key, std::move(value));
    }

    template <typename T>
    std::optional<T> get_copy(const OresExecutionClaim& current,
                              const RequestLocalKeyV1<T>& key) const {
        if (!authorized(current)) return std::nullopt;
        auto snapshot = request_->get_copy(key);
        // STOP may race the copy-out. The request store rechecks request
        // cancellation; this second gate also rechecks physical actor phase
        // and original-mail provenance before publishing a snapshot.
        if (!snapshot || !authorized(current)) return std::nullopt;
        return snapshot;
    }

    template <typename T>
    bool remove(const OresExecutionClaim& current,
                const RequestLocalKeyV1<T>& key) {
        if (!authorized(current)) return false;
        return request_->remove(key);
    }

private:
    OresNativeExecutionAuthority* const authority_;
    const RequestContextLocalV1::Ptr request_;
    const OresNativeActorRef actor_;
    const std::uint64_t admitted_mail_sequence_;
    const TrustedRequestMailReaderV1 current_mail_;
    void* const scheduler_context_;
};

} // namespace oreslang::runtime
