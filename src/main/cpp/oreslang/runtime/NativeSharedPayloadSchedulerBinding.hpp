#pragma once

#include "oreslang/runtime/NativeActorScheduler.h"
#include "oreslang/runtime/SharedImmutablePayload.hpp"

#include <cstddef>
#include <cstdint>

namespace oreslang::runtime {

// Trusted native host adapter only. Not a source .atto Channel, FFI pointer,
// or proof of zero-copy compiler lowering. The inbox is a separately-owned
// runtime resource and MUST outlive every in-flight scheduler callback.
struct NativeSharedPayloadSchedulerBinding {
    OresNativeActorScheduler* scheduler = nullptr;
    OresNativeActorRef actor{};
    SharedPayloadInbox* inbox = nullptr;
};

struct NativeSharedPayloadCommitContext {
    SharedPayloadInbox* inbox;
    const SharedImmutablePayload* payload;
};

// Stable externally-linked symbol across all C++ translation units. Its
// address is pinned by NativeActorScheduler during atomic pristine binding.
extern "C" int ores_native_shared_payload_commit(
    void* opaque, std::uint64_t source_mail_sequence) noexcept;

// Unlike a plain read-after-ordinal lookup, this callback executes under the
// same native scheduler mutex as producer admission (scheduler -> inbox).
// A delivered DATA turn must not see a transient BUSY from competing sends.
struct NativeSharedPayloadReadContext {
    SharedPayloadInbox* inbox;
    SharedPayloadInbox::ReceiveResult* output;
};
extern "C" int ores_native_shared_payload_read(
    void* opaque, std::uint64_t source_mail_sequence) noexcept;

inline int native_shared_payload_bind_once(void* opaque) noexcept {
    auto* inbox = static_cast<SharedPayloadInbox*>(opaque);
    if (!inbox) return 0;
    try {
        return inbox->try_bind_sequenced() ? 1 : 0;
    } catch (...) {
        return 0; // C++ exceptions must NEVER unwind into C scheduler lock.
    }
}

inline bool native_shared_payload_bind(
        NativeSharedPayloadSchedulerBinding* out,
        OresNativeActorScheduler* scheduler, OresNativeActorRef actor,
        SharedPayloadInbox* inbox) {
    if (!out || !scheduler || !inbox || !actor.scheduler_domain
            || !actor.generation || actor.actor_slot == UINT64_MAX)
        return false;
    // The scheduler proves the exact live actor generation and pristine DATA
    // lane BEFORE it binds the private sequenced descriptor queue.
    if (!ores_native_actor_scheduler_bind_bytes_only(
            scheduler, actor, ores_native_shared_payload_commit,
            native_shared_payload_bind_once, inbox))
        return false;
    *out = {scheduler, actor, inbox};
    return true;
}

inline OresNativeSendStatus native_shared_payload_try_send_atomic(
        const NativeSharedPayloadSchedulerBinding& binding,
        std::uint64_t control_value,
        const SharedImmutablePayload& payload) {
    if (!binding.scheduler || !binding.inbox || !payload.valid())
        return ORES_NATIVE_SEND_INVALID_REF;
    NativeSharedPayloadCommitContext context{binding.inbox, &payload};
    return ores_native_actor_scheduler_try_send_committed(
        binding.scheduler, binding.actor, control_value,
        ores_native_shared_payload_commit, &context);
}

inline SharedPayloadInbox::ReceiveResult native_shared_payload_receive_current_data(
        const NativeSharedPayloadSchedulerBinding& binding) {
    SharedPayloadInbox::ReceiveResult result{
        SharedPayloadInbox::ReceiveStatus::denied, std::nullopt
    };
    if (!binding.scheduler || !binding.inbox) return result;
    NativeSharedPayloadReadContext context{binding.inbox, &result};
    // Authenticates actor/generation/physical DATA ordinal, then serializes
    // the descriptor read against all committed producer publications.
    if (!ores_native_actor_scheduler_read_current_bytes(
            binding.scheduler, binding.actor,
            ores_native_shared_payload_read, &context))
        return {SharedPayloadInbox::ReceiveStatus::denied, std::nullopt};
    return result;
}

} // namespace oreslang::runtime
