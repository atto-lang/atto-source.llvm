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
 * Compiles the exact typed `on_panic(Failure) { self.stop(); return; }`
 * subset, then installs the resulting immutable decision policy on the
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
         requires_typed_failure = plan.explicitly_bound_failure]
         (const SupervisorFailureNotice& notice)
                -> std::optional<SupervisorProcess::Directive> {
            if (notice.supervisor_id != supervisor_id
                    || notice.runtime_id != runtime_id
                    || notice.actor_id == 0 || notice.generation == 0) {
                throw std::invalid_argument("foreign or stale typed Failure");
            }
            // The explicit guest Failure binder requires an actual typed
            // PANIC envelope. Legacy zero-kind host notices are supported
            // solely by the historical no-argument STOP form.
            if (requires_typed_failure
                    && notice.native_terminal_kind != ORES_NATIVE_CONTROL_PANIC) {
                throw std::invalid_argument(
                    "explicit Failure STOP requires typed PANIC envelope");
            }
            if (notice.native_terminal_kind != 0
                    && notice.native_terminal_kind != ORES_NATIVE_CONTROL_PANIC) {
                throw std::invalid_argument(
                    "source STOP-only on_panic cannot handle cancellation");
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
