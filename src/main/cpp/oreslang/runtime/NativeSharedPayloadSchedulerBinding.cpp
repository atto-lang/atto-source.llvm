#include "oreslang/runtime/NativeSharedPayloadSchedulerBinding.hpp"

namespace oreslang::runtime {

// Callback is entered with the native scheduler admission mutex held.
// It must never acquire the scheduler again, park or allocate queue nodes.
// Shared reference-count increments are atomic and the ring was preallocated.
// Exceptions are contained before returning to the C runtime.
extern "C" int ores_native_shared_payload_commit(
        void* opaque, std::uint64_t source_mail_sequence) noexcept {
    auto* context = static_cast<NativeSharedPayloadCommitContext*>(opaque);
    if (!context || !context->inbox || !context->payload || !source_mail_sequence)
        return 0;
    try {
        return context->inbox->try_send_sequenced(
            *context->payload, source_mail_sequence)
            == SharedPayloadInbox::SendResult::admitted ? 1 : 0;
    } catch (...) {
        return 0;
    }
}

// The scheduler has already verified the current physical DATA turn and
// holds its admission lock, so the descriptor inbox read cannot race a
// producer attempting to acquire the inbox try-lock under the same order.
// Return 1 when the callback ran, even if the inbox itself reports DENIED;
// that status is preserved in context->output for the trusted caller.
extern "C" int ores_native_shared_payload_read(
        void* opaque, std::uint64_t source_mail_sequence) noexcept {
    auto* context = static_cast<NativeSharedPayloadReadContext*>(opaque);
    if (!context || !context->inbox || !context->output || !source_mail_sequence)
        return 0;
    try {
        *context->output =
            context->inbox->try_receive_sequenced(source_mail_sequence);
        return 1;
    } catch (...) {
        *context->output = {
            SharedPayloadInbox::ReceiveStatus::denied, std::nullopt
        };
        return 0; // C++ exceptions must not cross the scheduler C lock.
    }
}

} // namespace oreslang::runtime
