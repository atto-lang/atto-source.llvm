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
    root.allocation = {ORES_ALLOCATION_DOMAIN_ACTOR, 77, 0, 9};
    OresNativeExecutionHandle execution{};
    CHECK(ores_native_execution_context_store_insert(executions, &root, &execution));

    OresNativeContinuationHandle continuation{};
    CHECK(ores_native_continuation_store_publish(
        store, execution, OresNativeContinuationHandle{},
        ORES_CONTINUATION_BOUNDARY_AWAIT, 7, 1001, &continuation));

    const NativeSourceCleanupTicket ticket{
        77, 9, ores_native_store_handle_pack(continuation)
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
        NativeSourceCleanupTicket{88, 9, ticket.continuation_id}, 7, 1001).claimed());
    CHECK(!NativeSourceCleanupBinding(store, continuation,
        NativeSourceCleanupTicket{77, 10, ticket.continuation_id}, 7, 1001).claimed());
    CHECK(!NativeSourceCleanupBinding(store, continuation,
        NativeSourceCleanupTicket{77, 9, ticket.continuation_id + 1}, 7, 1001).claimed());

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

    CHECK(ores_native_continuation_store_release(store, continuation));
    CHECK(!binding.claimed()); // Freed/stale handle cannot revive.
    CHECK(ores_native_continuation_store_close(store));
    CHECK(ores_native_execution_context_store_release(executions, execution));
    CHECK(ores_native_execution_context_store_close(executions));
}
