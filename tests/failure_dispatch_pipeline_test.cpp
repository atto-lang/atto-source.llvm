#include "oreslang/runtime/FailureDispatchPipeline.hpp"

#include <array>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#define CHECK(expr) do { if (!(expr)) throw std::runtime_error("failed: " #expr); } while (false)

namespace {
using namespace oreslang;
using Lane = SupervisorFailureLane;
using Key = Lane::ActorKey;

struct ParentHooks final : SupervisorControlTurn::Hooks {
    int callbacks = 0;
    Lane::Kind last_kind = Lane::Kind::Throw;
    Lane::Kind last_origin = Lane::Kind::Throw;
    std::uint32_t last_code = 0;
    std::string last_summary;

    SupervisorControlTurn::Decision record(
            Lane::Kind kind, const Lane::Failure& failure) {
        ++callbacks;
        last_kind = kind;
        last_origin = failure.origin_kind;
        last_code = failure.origin_code;
        last_summary = failure.summary;
        return SupervisorControlTurn::Decision::Acknowledge;
    }

    SupervisorControlTurn::Decision on_throw(const Lane::Failure& f) override {
        return record(Lane::Kind::Throw, f);
    }
    SupervisorControlTurn::Decision on_raise(const Lane::Failure& f) override {
        return record(Lane::Kind::Raise, f);
    }
    SupervisorControlTurn::Decision on_panic(const Lane::Failure& f) override {
        return record(Lane::Kind::Panic, f);
    }
    SupervisorControlTurn::Decision on_cancel(const Lane::Failure& f) override {
        return record(Lane::Kind::Cancel, f);
    }
    SupervisorControlTurn::Decision on_host_failure(const Lane::Failure& f) override {
        return record(Lane::Kind::HostFailure, f);
    }
    void on_escalate(const Lane::Failure&) override {}
    bool on_restart(const Lane::Failure&) override { return false; }
    void on_user_turn() override {}
};

struct HookState {
    FailureDispatchScratch* scratch = nullptr;
    int throws = 0;
    int raises = 0;
    bool saw_trace_before_hook = false;
};

ActorLocalHookReply handle_throw(void* raw, EffectStatus failure) noexcept {
    auto* state = static_cast<HookState*>(raw);
    ++state->throws;
    state->saw_trace_before_hook =
        state->scratch && state->scratch->trace_captured
        && state->scratch->trace.kind == ORES_FAILURE_TRACE_THROW
        && state->scratch->trace.code == failure.code;
    return {ActorLocalResolution::Handled, {EffectKind::Return, 0}};
}

ActorLocalHookReply escalate_raise(void* raw, EffectStatus failure) noexcept {
    auto* state = static_cast<HookState*>(raw);
    ++state->raises;
    state->saw_trace_before_hook =
        state->scratch && state->scratch->trace_captured
        && state->scratch->trace.kind == ORES_FAILURE_TRACE_RAISE
        && state->scratch->trace.code == failure.code;
    return {ActorLocalResolution::Escalate, {EffectKind::Return, 0}};
}

ActorLocalHookReply panic_from_throw_hook(void* raw, EffectStatus failure) noexcept {
    auto* state = static_cast<HookState*>(raw);
    ++state->throws;
    state->saw_trace_before_hook =
        state->scratch && state->scratch->trace_captured
        && state->scratch->trace.kind == ORES_FAILURE_TRACE_THROW
        && state->scratch->trace.code == failure.code;
    return {ActorLocalResolution::Handled, {EffectKind::Panic, 202}};
}

EffectStatus defer_panic(void*) noexcept {
    return {EffectKind::Panic, 99};
}

Key key(int n) {
    return {"failure-pipeline-" + std::to_string(n), 1};
}

void dispatch_parent(
        SupervisorControlTurn& parent, ParentHooks& hooks,
        Lane::Kind expected_kind, Lane::Kind expected_origin,
        std::uint32_t expected_code, const char* expected_text) {
    CHECK(parent.pending_count() == 1);
    CHECK(parent.admit_turn(hooks, false) == SupervisorControlTurn::Admitted::Control);
    CHECK(hooks.last_kind == expected_kind);
    CHECK(hooks.last_origin == expected_origin);
    CHECK(hooks.last_code == expected_code);
    CHECK(hooks.last_summary.find(expected_text) != std::string::npos);
    CHECK(parent.pending_count() == 0);
}

} // namespace

int main() {
    OresNativeExecutionContextStore* executions =
        ores_native_execution_context_store_open(32);
    CHECK(executions != nullptr);
    OresNativeContinuationStore* continuations =
        ores_native_continuation_store_open(32, executions);
    CHECK(continuations != nullptr);

    OresNativeExecutionContext origin{
        {77, 1},
        {ORES_ALLOCATION_DOMAIN_ACTOR, 77, 5, 9},
        {0, 0},
        {0, 0}
    };
    OresNativeExecutionHandle first{0, 0};
    CHECK(ores_native_execution_context_store_insert(executions, &origin, &first));

    OresNativeSuspensionLink link{};
    CHECK(ores_native_await_trace_suspend(
        executions, continuations, first, {0, 0},
        ORES_CONTINUATION_BOUNDARY_AWAIT, 4, 123, &link));
    CHECK(ores_native_execution_context_store_release(executions, first));

    OresNativeExecutionHandle resumed{0, 0};
    OresNativeExecutionContext resumed_context{};
    CHECK(ores_native_await_trace_resume_begin(
        executions, continuations, {77, 2}, link,
        &resumed, &resumed_context));
    CHECK(ores_native_await_trace_resume_finish(continuations, link));

    SupervisorControlTurn parent(16);
    ParentHooks parent_hooks;

    // 1. Defer/finally has already selected Panic before trace capture.
    // The original Throw is retained as a suppressed diagnostic.
    {
        const Key actor = key(1);
        parent.reserve_child(actor);
        EffectUnwind<1> frame;
        CHECK(frame.push(defer_panic, nullptr));
        FailureDispatchScratch scratch;
        const auto result = FailureDispatchPipeline::finish_capture_dispatch(
            frame, {EffectKind::Throw, 1},
            executions, continuations, resumed, scratch,
            nullptr, nullptr, nullptr, parent, actor, "root");
        CHECK(result.after_unwind.kind == EffectKind::Panic);
        CHECK(result.after_unwind.code == 99);
        CHECK(result.suppressed_count == 1);
        CHECK(result.supervisor_published);
        CHECK(scratch.trace_captured && !scratch.trace_capture_failed);
        CHECK(scratch.trace.kind == ORES_FAILURE_TRACE_PANIC);
        CHECK(scratch.trace.code == 99);
        CHECK(scratch.diagnostic_view().find("boundary await source=123") != std::string_view::npos);
        CHECK(scratch.diagnostic_view().find("suppressed throw code=1") != std::string_view::npos);
        dispatch_parent(parent, parent_hooks, Lane::Kind::Panic,
                        Lane::Kind::Panic, 99, "panic code=99");
    }

    // 2. Trace exists before an actor-local Throw hook. A handled failure
    // publishes nothing and the child reservation remains live.
    {
        const Key actor = key(2);
        parent.reserve_child(actor);
        EffectUnwind<0> frame;
        FailureDispatchScratch scratch;
        HookState state{&scratch};
        const auto result = FailureDispatchPipeline::finish_capture_dispatch(
            frame, {EffectKind::Throw, 11},
            executions, continuations, resumed, scratch,
            &state, handle_throw, escalate_raise, parent, actor);
        CHECK(result.after_unwind.kind == EffectKind::Throw);
        CHECK(result.actor_local.hook_invoked && result.actor_local.handled);
        CHECK(!result.supervisor_published);
        CHECK(state.throws == 1 && state.saw_trace_before_hook);
        CHECK(parent.pending_count() == 0);
        parent.retire_child(actor);
    }

    // 3. Unhandled Raise is traced as Raise, then promoted only at the
    // actor/supervisor boundary to Panic(origin=Raise).
    {
        const Key actor = key(3);
        parent.reserve_child(actor);
        EffectUnwind<0> frame;
        FailureDispatchScratch scratch;
        HookState state{&scratch};
        const auto result = FailureDispatchPipeline::finish_capture_dispatch(
            frame, {EffectKind::Raise, 12},
            executions, continuations, resumed, scratch,
            &state, handle_throw, escalate_raise, parent, actor);
        CHECK(result.supervisor_published);
        CHECK(state.raises == 1 && state.saw_trace_before_hook);
        CHECK(scratch.trace.kind == ORES_FAILURE_TRACE_RAISE);
        dispatch_parent(parent, parent_hooks, Lane::Kind::Panic,
                        Lane::Kind::Raise, 12, "raise code=12");
    }

    // 4. If the actor-local hook itself panics, the pre-hook trace still
    // preserves the original Throw while supervisor CONTROL reports Panic.
    {
        const Key actor = key(4);
        parent.reserve_child(actor);
        EffectUnwind<0> frame;
        FailureDispatchScratch scratch;
        HookState state{&scratch};
        const auto result = FailureDispatchPipeline::finish_capture_dispatch(
            frame, {EffectKind::Throw, 14},
            executions, continuations, resumed, scratch,
            &state, panic_from_throw_hook, nullptr, parent, actor);
        CHECK(result.actor_local.outgoing.kind == EffectKind::Panic);
        CHECK(result.actor_local.outgoing.code == 202);
        CHECK(result.supervisor_published);
        CHECK(state.saw_trace_before_hook);
        CHECK(scratch.trace.kind == ORES_FAILURE_TRACE_THROW);
        dispatch_parent(parent, parent_hooks, Lane::Kind::Panic,
                        Lane::Kind::Panic, 202, "throw code=14");
    }

    // 5. ProcessExit remains diagnostically distinct even though supervisor
    // policy maps it to HostFailure at the actor boundary.
    {
        const Key actor = key(5);
        parent.reserve_child(actor);
        EffectUnwind<0> frame;
        FailureDispatchScratch scratch;
        const auto result = FailureDispatchPipeline::finish_capture_dispatch(
            frame, {EffectKind::ProcessExit, 0},
            executions, continuations, resumed, scratch,
            nullptr, nullptr, nullptr, parent, actor);
        CHECK(result.supervisor_published);
        CHECK(scratch.trace.kind == ORES_FAILURE_TRACE_PROCESS_EXIT);
        dispatch_parent(parent, parent_hooks, Lane::Kind::HostFailure,
                        Lane::Kind::HostFailure, 0, "process-exit code=0");
    }

    // 6. A forged native tag is normalized during unwind before capture and
    // cannot masquerade as a Throw/Return or bypass the supervisor.
    {
        const Key actor = key(6);
        parent.reserve_child(actor);
        EffectUnwind<0> frame;
        FailureDispatchScratch scratch;
        const auto result = FailureDispatchPipeline::finish_capture_dispatch(
            frame, {static_cast<EffectKind>(0xff), 7},
            executions, continuations, resumed, scratch,
            nullptr, nullptr, nullptr, parent, actor);
        CHECK(result.after_unwind.kind == EffectKind::Fatal);
        CHECK(result.after_unwind.code == 0xFFFFFFFDu);
        CHECK(scratch.trace.kind == ORES_FAILURE_TRACE_FATAL);
        CHECK(result.supervisor_published);
        dispatch_parent(parent, parent_hooks, Lane::Kind::HostFailure,
                        Lane::Kind::HostFailure, 0xFFFFFFFDu,
                        "fatal code=4294967293");
    }

    // 7. Concurrent carriers may capture the same current logical execution.
    // Each capture pins it independently and publishes to a distinct reserved
    // supervisor slot; no stale-root race or lost CONTROL event is allowed.
    {
        constexpr std::size_t count = 8;
        std::array<Key, count> actors{{
            key(100), key(101), key(102), key(103),
            key(104), key(105), key(106), key(107)
        }};
        for (const auto& actor : actors) parent.reserve_child(actor);

        std::array<bool, count> published{};
        std::array<bool, count> captured{};
        std::array<std::thread, count> threads;
        for (std::size_t i = 0; i < count; ++i) {
            threads[i] = std::thread([&, i] {
                EffectUnwind<0> frame;
                FailureDispatchScratch scratch;
                const auto result = FailureDispatchPipeline::finish_capture_dispatch(
                    frame,
                    {EffectKind::Panic, static_cast<std::uint32_t>(300 + i)},
                    executions, continuations, resumed, scratch,
                    nullptr, nullptr, nullptr, parent, actors[i], "root");
                published[i] = result.supervisor_published;
                captured[i] = scratch.trace_captured
                    && !scratch.trace_capture_failed
                    && scratch.trace.kind == ORES_FAILURE_TRACE_PANIC;
            });
        }
        for (auto& thread : threads) thread.join();
        for (std::size_t i = 0; i < count; ++i) {
            CHECK(published[i]);
            CHECK(captured[i]);
        }
        CHECK(parent.pending_count() == count);
        const int before = parent_hooks.callbacks;
        for (std::size_t i = 0; i < count; ++i) {
            CHECK(parent.admit_turn(parent_hooks, false)
                == SupervisorControlTurn::Admitted::Control);
            CHECK(parent_hooks.last_kind == Lane::Kind::Panic);
            CHECK(parent_hooks.last_origin == Lane::Kind::Panic);
            CHECK(parent_hooks.last_summary.find("panic code=") == 0);
        }
        CHECK(parent_hooks.callbacks == before + static_cast<int>(count));
        CHECK(parent.pending_count() == 0);
    }

    // 8. Diagnostic failure must not suppress terminal control flow. A stale
    // current execution yields a bounded fallback summary and still publishes
    // the original Throw through CONTROL.
    {
        OresNativeExecutionContext temporary{
            {77, 99},
            {ORES_ALLOCATION_DOMAIN_ACTOR, 77, 5, 9},
            {0, 0},
            {0, 0}
        };
        OresNativeExecutionHandle stale{0, 0};
        CHECK(ores_native_execution_context_store_insert(
            executions, &temporary, &stale));
        CHECK(ores_native_execution_context_store_release(executions, stale));

        const Key actor = key(7);
        parent.reserve_child(actor);
        EffectUnwind<0> frame;
        FailureDispatchScratch scratch;
        const auto result = FailureDispatchPipeline::finish_capture_dispatch(
            frame, {EffectKind::Throw, 88},
            executions, continuations, stale, scratch,
            nullptr, nullptr, nullptr, parent, actor);
        CHECK(!scratch.trace_captured && scratch.trace_capture_failed);
        CHECK(result.supervisor_published);
        CHECK(scratch.diagnostic_view().find(
            "logical failure trace unavailable") != std::string_view::npos);
        dispatch_parent(parent, parent_hooks, Lane::Kind::Panic,
                        Lane::Kind::Throw, 88,
                        "logical failure trace unavailable");
    }

    CHECK(ores_native_execution_context_store_release(executions, resumed));
    CHECK(ores_native_continuation_store_release(continuations, link.continuation));
    CHECK(ores_native_continuation_store_live(continuations) == 0);
    CHECK(ores_native_execution_context_store_live(executions) == 0);
    CHECK(ores_native_continuation_store_close(continuations));
    CHECK(ores_native_execution_context_store_close(executions));
    std::puts("FAILURE_DISPATCH_PIPELINE_PASS");
    return 0;
}
