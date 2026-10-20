#pragma once

#include "oreslang/runtime/ActorLocalFailurePolicy.hpp"
#include "oreslang/runtime/EffectSupervisorBridge.hpp"
#include "oreslang/runtime/NativeFailureTrace.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string_view>

namespace oreslang {

/*
 * One bounded failure envelope spans exactly one post-unwind dispatch.
 * It owns only value metadata and caller-local buffers; no guest/native stack
 * pointer, exception object or heap allocation is retained.
 */
struct FailureDispatchScratch final {
    static constexpr std::size_t execution_capacity = 8;
    static constexpr std::size_t continuation_capacity = 8;
    static constexpr std::size_t diagnostic_capacity = 512;

    OresNativeFailureTraceReport trace{};
    std::array<OresNativeExecutionTraceFrame, execution_capacity> executions{};
    std::array<OresNativeContinuationTraceFrame, continuation_capacity> continuations{};
    std::array<char, diagnostic_capacity> diagnostic{};
    bool trace_captured = false;
    bool trace_capture_failed = false;
    bool diagnostic_truncated = false;

    std::string_view diagnostic_view() const noexcept {
        std::size_t length = 0;
        while (length < diagnostic.size() && diagnostic[length] != '\0') ++length;
        return {diagnostic.data(), length};
    }
};

struct FailureDispatchResult final {
    EffectStatus after_unwind{};
    ActorLocalDispatch actor_local{};
    std::size_t suppressed_count = 0;
    std::size_t suppressed_overflow = 0;
    bool supervisor_published = false;
};

class FailureDispatchPipeline final {
public:
    /*
     * The ordering is intentional and non-optional:
     *
     *   lexical defer/finally unwind
     *     -> capture effective failure trace
     *     -> actor-local on_throw/on_raise
     *     -> supervisor CONTROL promotion/publication
     *
     * A handled actor-local Throw/Raise keeps the pre-reserved child slot for
     * continued execution; no supervisor event is published.
     */
    template <typename UnwindFrame>
    static FailureDispatchResult finish_capture_dispatch(
            UnwindFrame& unwind_frame,
            EffectStatus incoming,
            OresNativeExecutionContextStore* executions,
            OresNativeContinuationStore* continuations,
            OresNativeExecutionHandle current_execution,
            FailureDispatchScratch& scratch,
            void* actor_hook_state,
            ActorLocalHook on_throw,
            ActorLocalHook on_raise,
            SupervisorControlTurn& supervisor,
            const SupervisorFailureLane::ActorKey& actor,
            std::string_view parent_id = {}) noexcept {
        reset(scratch);
        FailureDispatchResult result{};

        const auto completed = unwind_frame.finish(incoming);
        result.after_unwind = normalize_effect(completed.primary);
        result.suppressed_count = completed.secondary_count;
        result.suppressed_overflow = completed.overflow_count;

        if (result.after_unwind.kind == EffectKind::Return) {
            result.actor_local = {false, false, result.after_unwind};
            return result;
        }

        capture_completed_failure(
            result.after_unwind,
            completed,
            executions,
            continuations,
            current_execution,
            scratch);

        result.actor_local = dispatch_actor_local_unhandled(
            result.after_unwind,
            actor_hook_state,
            on_throw,
            on_raise);

        if (result.actor_local.handled
                || result.actor_local.outgoing.kind == EffectKind::Return)
            return result;

        /*
         * The trace describes the failure selected by lexical unwind. If an
         * actor-local hook itself escalates (for example Throw -> Panic), the
         * supervisor gets the hook's terminal typed effect while the diagnostic
         * still preserves the original post-unwind trace.
         */
        result.supervisor_published = EffectSupervisorBridge::publish_terminal(
            supervisor,
            actor,
            result.actor_local.outgoing,
            scratch.diagnostic_view(),
            parent_id);
        return result;
    }

private:
    static void reset(FailureDispatchScratch& scratch) noexcept {
        scratch = FailureDispatchScratch{};
    }

    static OresNativeFailureTraceKind trace_kind(EffectKind kind) noexcept {
        switch (kind) {
            case EffectKind::Throw: return ORES_FAILURE_TRACE_THROW;
            case EffectKind::Raise: return ORES_FAILURE_TRACE_RAISE;
            case EffectKind::Panic: return ORES_FAILURE_TRACE_PANIC;
            case EffectKind::Cancel: return ORES_FAILURE_TRACE_CANCEL;
            case EffectKind::Fatal: return ORES_FAILURE_TRACE_FATAL;
            case EffectKind::ProcessExit: return ORES_FAILURE_TRACE_PROCESS_EXIT;
            case EffectKind::Return: break;
        }
        return ORES_FAILURE_TRACE_FATAL;
    }

    static const char* effect_name(EffectKind kind) noexcept {
        switch (kind) {
            case EffectKind::Return: return "return";
            case EffectKind::Throw: return "throw";
            case EffectKind::Raise: return "raise";
            case EffectKind::Panic: return "panic";
            case EffectKind::Cancel: return "cancel";
            case EffectKind::Fatal: return "fatal";
            case EffectKind::ProcessExit: return "process-exit";
        }
        return "fatal";
    }

    static void fallback_diagnostic(
            EffectStatus effect,
            FailureDispatchScratch& scratch) noexcept {
        const int n = std::snprintf(
            scratch.diagnostic.data(),
            scratch.diagnostic.size(),
            "%s code=%u\n  logical failure trace unavailable\n",
            effect_name(effect.kind),
            static_cast<unsigned>(effect.code));
        if (n < 0) {
            scratch.diagnostic[0] = '\0';
            scratch.diagnostic_truncated = true;
            return;
        }
        scratch.diagnostic_truncated =
            static_cast<std::size_t>(n) >= scratch.diagnostic.size();
        scratch.diagnostic.back() = '\0';
    }

    template <typename Summary>
    static void append_suppressed(
            const Summary& completed,
            FailureDispatchScratch& scratch) noexcept {
        std::size_t used = 0;
        while (used < scratch.diagnostic.size()
                && scratch.diagnostic[used] != '\0') ++used;
        if (used == scratch.diagnostic.size()) {
            scratch.diagnostic.back() = '\0';
            used = scratch.diagnostic.size() - 1;
            scratch.diagnostic_truncated = true;
        }

        for (std::size_t i = 0; i < completed.secondary_count; ++i) {
            if (used >= scratch.diagnostic.size() - 1) {
                scratch.diagnostic_truncated = true;
                break;
            }
            const auto effect = normalize_effect(completed.secondary[i]);
            const int n = std::snprintf(
                scratch.diagnostic.data() + used,
                scratch.diagnostic.size() - used,
                "  suppressed %s code=%u\n",
                effect_name(effect.kind),
                static_cast<unsigned>(effect.code));
            if (n < 0) {
                scratch.diagnostic_truncated = true;
                break;
            }
            const std::size_t needed = static_cast<std::size_t>(n);
            if (needed >= scratch.diagnostic.size() - used) {
                scratch.diagnostic_truncated = true;
                used = scratch.diagnostic.size() - 1;
                scratch.diagnostic[used] = '\0';
                break;
            }
            used += needed;
        }

        if (completed.overflow_count != 0 && used < scratch.diagnostic.size() - 1) {
            const int n = std::snprintf(
                scratch.diagnostic.data() + used,
                scratch.diagnostic.size() - used,
                "  suppressed-overflow=%zu\n",
                completed.overflow_count);
            if (n < 0 || static_cast<std::size_t>(n) >= scratch.diagnostic.size() - used)
                scratch.diagnostic_truncated = true;
        }
    }

    template <typename Summary>
    static void capture_completed_failure(
            EffectStatus effect,
            const Summary& completed,
            OresNativeExecutionContextStore* executions,
            OresNativeContinuationStore* continuations,
            OresNativeExecutionHandle current_execution,
            FailureDispatchScratch& scratch) noexcept {
        if (!ores_native_failure_trace_capture(
                executions,
                continuations,
                current_execution,
                trace_kind(effect.kind),
                effect.code,
                scratch.executions.data(),
                scratch.executions.size(),
                scratch.continuations.data(),
                scratch.continuations.size(),
                &scratch.trace)) {
            scratch.trace_capture_failed = true;
            fallback_diagnostic(effect, scratch);
            append_suppressed(completed, scratch);
            return;
        }

        scratch.trace_captured = true;
        if (!ores_native_failure_trace_render(
                &scratch.trace,
                scratch.executions.data(),
                scratch.continuations.data(),
                scratch.diagnostic.data(),
                scratch.diagnostic.size())) {
            scratch.trace_capture_failed = true;
            fallback_diagnostic(effect, scratch);
        } else {
            scratch.diagnostic_truncated = scratch.trace.text_truncated != 0;
        }
        append_suppressed(completed, scratch);
    }
};

} // namespace oreslang
