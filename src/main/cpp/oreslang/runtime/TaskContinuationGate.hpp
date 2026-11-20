#pragma once

#include "oreslang/runtime/NativeAwaitTraceBridge.h"
#include "oreslang/runtime/TaskCancellation.hpp"

#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace oreslang::runtime {

/*
 * Runtime-private bridge between TaskCancellation and a suspended native
 * continuation. The token is an exact generation-checked SuspensionLink.
 *
 * The underlying C stores are borrowed. The caller MUST keep both stores
 * alive, call close() and leave all resumed native frames before releasing
 * the link or destroying the stores. close() cancels any still-unclaimed
 * continuation; a CLAIMED continuation must be finished by its owner.
 *
 * This is a host scheduler primitive, NOT a guest Task/Future<T> ABI.
 */
class TaskContinuationGate final {
    struct Shared final {
        std::mutex mutex;
        TaskCancellation::Ptr authority;
        OresNativeExecutionContextStore* executions;
        OresNativeContinuationStore* continuations;
        OresNativeSuspensionLink link;
        bool open = true;
        bool owns_claim = false;

        bool revoke() noexcept {
            std::lock_guard<std::mutex> guard(mutex);
            if (!open) return false;
            return ores_native_continuation_store_cancel(
                continuations, link.continuation) != 0;
        }
    };

public:
    TaskContinuationGate(
            TaskCancellation::Ptr authority,
            OresNativeExecutionContextStore* executions,
            OresNativeContinuationStore* continuations,
            OresNativeSuspensionLink link,
            uint64_t expected_code_generation,
            OresNativeAllocationDomainRef expected_allocation,
            OresNativeContinuationBoundary expected_boundary =
                ORES_CONTINUATION_BOUNDARY_AWAIT) {
        if (!authority || !executions || !continuations ||
                expected_code_generation == 0 ||
                expected_allocation.kind == ORES_ALLOCATION_DOMAIN_NONE ||
                (expected_boundary != ORES_CONTINUATION_BOUNDARY_AWAIT &&
                 expected_boundary != ORES_CONTINUATION_BOUNDARY_TASK_JOIN) ||
                !ores_native_store_handle_valid(link.parent) ||
                !ores_native_store_handle_valid(link.continuation))
            throw std::invalid_argument("invalid native task continuation binding");

        OresNativeContinuationRecord record{};
        if (!ores_native_continuation_store_get(
                continuations, link.continuation, &record) ||
                record.state != ORES_CONTINUATION_PENDING ||
                /*
                 * The default is AWAIT. Native pthread join adapters must
                 * explicitly opt in to TASK_JOIN; neither may borrow spawn,
                 * actor-message, CONTROL or each other's continuation.
                 */
                record.boundary != expected_boundary ||
                /*
                 * Reject a cross-code-reload or cross-actor/task reuse even
                 * when the logical execution handle itself is valid.
                 */
                record.code_generation != expected_code_generation ||
                record.allocation.kind != expected_allocation.kind ||
                record.allocation.domain != expected_allocation.domain ||
                record.allocation.owner != expected_allocation.owner ||
                record.allocation.generation != expected_allocation.generation ||
                record.execution.slot != link.parent.slot ||
                record.execution.generation != link.parent.generation ||
                record.execution_id.runtime_domain != link.execution_id.runtime_domain ||
                record.execution_id.execution != link.execution_id.execution)
            throw std::invalid_argument("stale or mismatched continuation binding");

        shared_ = std::make_shared<Shared>();
        shared_->authority = std::move(authority);
        shared_->executions = executions;
        shared_->continuations = continuations;
        shared_->link = link;

        std::weak_ptr<Shared> weak = shared_;
        revocation_ = shared_->authority->on_revoke([weak]() noexcept {
            if (auto active = weak.lock()) (void)active->revoke();
        });
        // on_revoke synchronously rejects a previously cancelled authority.
    }

    TaskContinuationGate(const TaskContinuationGate&) = delete;
    TaskContinuationGate& operator=(const TaskContinuationGate&) = delete;
    TaskContinuationGate(TaskContinuationGate&&) = delete;
    TaskContinuationGate& operator=(TaskContinuationGate&&) = delete;

    ~TaskContinuationGate() { close(); }

    bool notify_ready() noexcept {
        std::lock_guard<std::mutex> guard(shared_->mutex);
        if (!shared_->open) return false;
        /*
         * The Task graph lock protects this C store transition against the
         * exact cancellation/retirement marking boundary, not just eventual
         * delivery of the subscription revoker.
         */
        return shared_->authority->with_active_authority([&]() {
            return ores_native_continuation_store_notify_ready(
                shared_->continuations, shared_->link.continuation) != 0;
        });
    }

    bool try_resume(
            OresNativeExecutionId physical_execution,
            OresNativeExecutionHandle* out_execution,
            OresNativeExecutionContext* out_context = nullptr) noexcept {
        std::lock_guard<std::mutex> guard(shared_->mutex);
        if (!shared_->open) return false;
        // NativeAwaitTraceBridge atomically performs READY -> CLAIMED.
        // Holding Task authority admission across the claim means a retiring
        // or cancelled Task cannot be marked inactive and still enter guest.
        return shared_->authority->with_active_authority([&]() {
            const bool claimed = ores_native_await_trace_ready_resume_begin(
                shared_->executions, shared_->continuations,
                physical_execution, shared_->link, out_execution, out_context) != 0;
            if (claimed) shared_->owns_claim = true;
            return claimed;
        });
    }

    bool finish_resume() noexcept {
        std::lock_guard<std::mutex> guard(shared_->mutex);
        // An unrelated binding must never finish another Task's claimed
        // continuation. The original claiming gate retains physical cleanup
        // authority even if close() has revoked future admission.
        if (!shared_->owns_claim) return false;
        if (!ores_native_await_trace_resume_finish(
                shared_->continuations, shared_->link)) return false;
        shared_->owns_claim = false;
        return true;
    }

    void close() noexcept {
        if (!shared_) return;
        {
            std::lock_guard<std::mutex> guard(shared_->mutex);
            if (shared_->open) {
                shared_->open = false;
                // No active guest turn can be cancelled at CLAIMED, but the
                // pending/ready waiter must be revoked before store teardown.
                (void)ores_native_continuation_store_cancel(
                    shared_->continuations, shared_->link.continuation);
            }
        }
        // Never hold the private mutex while unregistering the hook.
        revocation_.close();
    }

private:
    std::shared_ptr<Shared> shared_;
    TaskCancellation::Subscription revocation_;
};

}  // namespace oreslang::runtime
