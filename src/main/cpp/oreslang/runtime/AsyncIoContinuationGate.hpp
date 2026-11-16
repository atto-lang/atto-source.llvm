#pragma once

#include "oreslang/runtime/AsyncIoOperationLifecycle.hpp"
#include "oreslang/runtime/NativeAwaitTraceBridge.h"

#include <cstdint>
#include <stdexcept>

namespace oreslang::runtime {

// Scheduler bridge for one native async-I/O operation.
//
// The reactor owns AsyncIoOperationLifecycle until physical retirement. Only
// after ready_to_reclaim() is true may this gate publish the suspended await
// continuation READY. That prevents guest resume from overlapping the native
// callback/registration that produced the completion.
class AsyncIoContinuationGate final {
public:
    AsyncIoContinuationGate(
            AsyncIoOperationLifecycle& lifecycle,
            OresNativeExecutionContextStore* executions,
            OresNativeContinuationStore* continuations,
            OresNativeSuspensionLink link,
            std::uint64_t expected_code_generation)
        : lifecycle_(lifecycle),
          executions_(executions),
          continuations_(continuations),
          link_(link) {
        if (!executions_ || !continuations_
                || expected_code_generation == 0
                || !ores_native_store_handle_valid(link_.parent)
                || !ores_native_store_handle_valid(link_.continuation))
            throw std::invalid_argument("invalid async I/O continuation binding");

        OresNativeContinuationRecord record{};
        if (!ores_native_continuation_store_get(
                continuations_, link_.continuation, &record)
                || record.state != ORES_CONTINUATION_PENDING
                || record.boundary != ORES_CONTINUATION_BOUNDARY_AWAIT
                || record.code_generation != expected_code_generation
                || record.execution.slot != link_.parent.slot
                || record.execution.generation != link_.parent.generation
                || record.execution_id.runtime_domain != link_.execution_id.runtime_domain
                || record.execution_id.execution != link_.execution_id.execution)
            throw std::invalid_argument("stale or mismatched async I/O continuation");

        const auto& ticket = lifecycle_.ticket();
        if (record.allocation.owner != ticket.actor_id
                || record.allocation.generation != ticket.generation
                || record.source_token != ticket.operation_id)
            throw std::invalid_argument("async I/O ticket does not match suspended continuation");
    }

    AsyncIoContinuationGate(const AsyncIoContinuationGate&) = delete;
    AsyncIoContinuationGate& operator=(const AsyncIoContinuationGate&) = delete;

    // Publish scheduler visibility only after the native operation frame is
    // physically reclaimable. READY still competes atomically with cancellation
    // in NativeContinuationStore.
    bool notify_ready_after_retirement() noexcept {
        if (!lifecycle_.ready_to_reclaim()) return false;
        return ores_native_continuation_store_notify_ready(
                continuations_, link_.continuation) != 0;
    }

    // Cancellation revokes the waiter first. If the native operation is still
    // pending, mark its logical operation terminal cancelled; the reactor owner
    // must still remove registration/end callbacks/retire before actor done.
    bool cancel() noexcept {
        const bool revoked = ores_native_continuation_store_cancel(
                continuations_, link_.continuation) != 0;
        if (revoked && lifecycle_.terminal() == AsyncIoTerminal::pending)
            (void) lifecycle_.cancel(lifecycle_.ticket());
        return revoked;
    }

    bool try_resume(
            OresNativeExecutionId resumed_execution_id,
            OresNativeExecutionHandle* out_execution,
            OresNativeExecutionContext* out_context = nullptr) noexcept {
        if (!lifecycle_.ready_to_reclaim()) return false;
        return ores_native_await_trace_ready_resume_begin(
                executions_, continuations_, resumed_execution_id, link_,
                out_execution, out_context) != 0;
    }

    bool finish_resume() noexcept {
        return ores_native_await_trace_resume_finish(
                continuations_, link_) != 0;
    }

private:
    AsyncIoOperationLifecycle& lifecycle_;
    OresNativeExecutionContextStore* executions_;
    OresNativeContinuationStore* continuations_;
    OresNativeSuspensionLink link_;
};

} // namespace oreslang::runtime
