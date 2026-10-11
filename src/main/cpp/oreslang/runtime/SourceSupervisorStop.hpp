#pragma once

#include "oreslang/frontend/Parser.hpp"
#include "oreslang/runtime/NativeActorScheduler.h"
#include "oreslang/runtime/SupervisorProcess.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace oreslang::runtime {

/**
 * First opt-in native guest-source-to-CONTROL binding.
 * Compiles the restricted `on_panic(f: Failure)` hook with either
 * `self.stop()` or `self.stop(f)` followed by `return;`, then installs the
 * resulting immutable decision policy on the
 * existing host-owned supervisor CONTROL process.
 *
 * The host sink must acknowledge STOP for an already finalized child; this
 * function does NOT synthesize actor restarts, joins, or escalation authority.
 */
inline std::shared_ptr<SupervisorProcess> install_source_supervisor_stop(
        std::string_view source,
        std::uint64_t supervisor_id,
        std::uint64_t runtime_id,
        std::size_t capacity,
        SupervisorProcess::Enqueue enqueue,
        SupervisorProcess::DecisionSink decision_sink) {
    const oreslang::StopOnlySupervisor plan =
        oreslang::parse_stop_only_supervisor(source);
    if (plan.name.empty()) {
        throw std::invalid_argument("missing compiled supervisor name");
    }
    return std::make_shared<SupervisorProcess>(
        supervisor_id, runtime_id, capacity,
        std::move(enqueue),
        [supervisor_id, runtime_id,
         targets_logical_child = plan.targets_logical_child]
         (const SupervisorFailureNotice& notice)
                -> std::optional<SupervisorProcess::Directive> {
            if (notice.supervisor_id != supervisor_id
                    || notice.runtime_id != runtime_id
                    || notice.actor_id == 0 || notice.generation == 0) {
                throw std::invalid_argument("foreign or stale typed Failure");
            }
            // BOTH source spellings, self.stop() and self.stop(f), are
            // compiled from on_panic(f: Failure) and require provenance from
            // a typed, finalized PANIC. A legacy zero-kind host notification
            // cannot manufacture a guest Failure merely by omitting f.
            if (notice.native_terminal_kind != ORES_NATIVE_CONTROL_PANIC) {
                throw std::invalid_argument(
                    "source STOP requires typed terminal PANIC envelope");
            }
            if (targets_logical_child
                    && (!notice.logical_child_id
                        || !notice.logical_incarnation)) {
                throw std::invalid_argument(
                    "source Failure.child STOP lacks verified logical child");
            }
            return SupervisorProcess::Directive::stop;
        },
        [supervisor_id, runtime_id, sink = std::move(decision_sink)](
                const SupervisorProcess::Resolution& resolution) {
            if (resolution.requested != SupervisorProcess::Directive::stop
                    || resolution.notice.supervisor_id != supervisor_id
                    || resolution.notice.runtime_id != runtime_id) {
                throw std::logic_error("invalid compiled source STOP decision");
            }
            sink(resolution);
        });
}

} // namespace oreslang::runtime
