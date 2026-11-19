#include "oreslang/runtime/NativeSourceCleanupBinding.hpp"
#include <stdexcept>

using namespace oreslang::runtime;
#define CHECK(e) do { if (!(e)) throw std::runtime_error(#e); } while (false)

int main() {
    auto* executions = ores_native_execution_context_store_open(8);
    CHECK(executions != nullptr);
    auto* store = ores_native_continuation_store_open(8, executions);
    CHECK(store != nullptr);

    OresNativeExecutionContext root{};
    root.id = {77, 101};
    root.allocation = {ORES_ALLOCATION_DOMAIN_ACTOR, 77, 17, 9};
    OresNativeExecutionHandle execution{};
    CHECK(ores_native_execution_context_store_insert(executions, &root, &execution));

    OresNativeContinuationHandle continuation{};
    CHECK(ores_native_continuation_store_publish(
        store, execution, OresNativeContinuationHandle{},
        ORES_CONTINUATION_BOUNDARY_AWAIT, 7, 1001, &continuation));

    const NativeSourceCleanupTicket ticket{
        17, 9, ores_native_store_handle_pack(continuation)
    };
    NativeSourceCleanupGate gate(ticket);
    NativeSourceCleanupBinding binding(store, continuation, ticket, 7, 1001);
    CHECK(!binding.claimed()); // PENDING is not a scheduler-owned CLAIMED turn.
    CHECK(gate.request_cancel(ticket));
    CHECK(!binding.claim_cleanup(gate));
    OresNativeContinuationRecord claimed{};
    CHECK(ores_native_continuation_store_try_claim_resume(
        store, continuation, &claimed));
    CHECK(binding.claimed());
    CHECK(!NativeSourceCleanupBinding(store, continuation, ticket, 8, 1001).claimed());
    CHECK(!NativeSourceCleanupBinding(store, continuation, ticket, 7, 1002).claimed());
    CHECK(!NativeSourceCleanupBinding(store, continuation,
        NativeSourceCleanupTicket{18, 9, ticket.continuation_id}, 7, 1001).claimed());
    CHECK(!NativeSourceCleanupBinding(store, continuation,
        NativeSourceCleanupTicket{17, 10, ticket.continuation_id}, 7, 1001).claimed());
    CHECK(!NativeSourceCleanupBinding(store, continuation,
        NativeSourceCleanupTicket{17, 9, ticket.continuation_id + 1}, 7, 1001).claimed());

    // The actor id is the allocation owner/slot, not the scheduler runtime
    // domain. A real scheduler context has distinct values (77 vs 17).
    CHECK(binding.claimed());

    // Adversarial alias: if a scheduler domain happens to equal another
    // actor's numeric id, that coincidence must not authorize cleanup for
    // the actor actually owning the continuation.
    OresNativeExecutionContext wrongOwner{};
    wrongOwner.id = {17, 201};
    wrongOwner.allocation = {ORES_ALLOCATION_DOMAIN_ACTOR, 17, 18, 9};
    OresNativeExecutionHandle wrongExecution{};
    CHECK(ores_native_execution_context_store_insert(
        executions, &wrongOwner, &wrongExecution));
    OresNativeContinuationHandle wrongContinuation{};
    CHECK(ores_native_continuation_store_publish(
        store, wrongExecution, OresNativeContinuationHandle{},
        ORES_CONTINUATION_BOUNDARY_AWAIT, 7, 3001, &wrongContinuation));
    OresNativeContinuationRecord wrongClaimed{};
    CHECK(ores_native_continuation_store_try_claim_resume(
        store, wrongContinuation, &wrongClaimed));
    const NativeSourceCleanupTicket forgedOwner{
        17, 9, ores_native_store_handle_pack(wrongContinuation)
    };
    CHECK(!NativeSourceCleanupBinding(
        store, wrongContinuation, forgedOwner, 7, 3001).claimed());

    auto* other = ores_native_continuation_store_open(2, executions);
    CHECK(other != nullptr);
    CHECK(!NativeSourceCleanupBinding(other, continuation, ticket, 7, 1001).claimed());
    CHECK(ores_native_continuation_store_close(other));

    CHECK(binding.claim_cleanup(gate));
    CHECK(!binding.claim_cleanup(gate));
    CHECK(binding.yield_cleanup(gate));
    CHECK(!gate.ready_for_reclaim());
    CHECK(binding.claim_cleanup(gate));
    CHECK(binding.complete_cleanup(gate));
    CHECK(gate.ready_for_reclaim());
    CHECK(ores_native_continuation_store_finish_resume(store, continuation));
    CHECK(!binding.claimed()); // COMPLETED no longer grants a cleanup turn.
    CHECK(!binding.complete_cleanup(gate));

    // Normal success is also a positive arena-reclaim receipt. It must be
    // bound to the claimed source continuation, not a forgeable ticket.
    OresNativeContinuationHandle normalContinuation{};
    CHECK(ores_native_continuation_store_publish(
        store, execution, OresNativeContinuationHandle{},
        ORES_CONTINUATION_BOUNDARY_AWAIT, 7, 1002, &normalContinuation));
    const NativeSourceCleanupTicket normalTicket{
        17, 9, ores_native_store_handle_pack(normalContinuation)
    };
    NativeSourceCleanupGate normalGate(normalTicket);
    NativeSourceCleanupBinding normalBinding(
        store, normalContinuation, normalTicket, 7, 1002);
    CHECK(!normalBinding.complete_normally(normalGate));
    OresNativeContinuationRecord normalClaimed{};
    CHECK(ores_native_continuation_store_try_claim_resume(
        store, normalContinuation, &normalClaimed));
    CHECK(!NativeSourceCleanupBinding(
        store, normalContinuation, normalTicket, 7, 1003)
        .complete_normally(normalGate));
    CHECK(normalBinding.complete_normally(normalGate));
    CHECK(normalGate.ready_for_reclaim());
    CHECK(!normalBinding.complete_normally(normalGate));
    CHECK(ores_native_continuation_store_finish_resume(
        store, normalContinuation));
    CHECK(!normalBinding.claimed());
    CHECK(ores_native_continuation_store_release(store, normalContinuation));

    CHECK(ores_native_continuation_store_finish_resume(
        store, wrongContinuation));
    CHECK(ores_native_continuation_store_release(store, wrongContinuation));
    CHECK(ores_native_execution_context_store_release(
        executions, wrongExecution));

    CHECK(ores_native_continuation_store_release(store, continuation));
    CHECK(!binding.claimed()); // Freed/stale handle cannot revive.
    CHECK(ores_native_continuation_store_close(store));
    CHECK(ores_native_execution_context_store_release(executions, execution));
    CHECK(ores_native_execution_context_store_close(executions));
}
