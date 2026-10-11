#pragma once

#include "oreslang/frontend/Parser.hpp"
#include "oreslang/runtime/NativeActorScheduler.h"
#include "oreslang/runtime/SupervisorChildRegistry.hpp"
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
    // A source-level logical-child projection requires a host registry
    // snapshot. Merely placing nonzero IDs in a typed notice is not proof
    // of ownership or finalization. Legacy host-only STOP is narrower.
    if (plan.targets_logical_child) {
        throw std::invalid_argument(
            "Failure.child STOP requires the registry-verified installer");
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

/**
 * Registry-bound source STOP for a lifecycle owner that can prove each
 * terminal Failure's exact finalized child incarnation.
 *
 * Unlike the legacy host-only overload, this opt-in boundary verifies the
 * current phase/actor identity before issuing STOP and again immediately
 * before the host effect. The host sink must execute the authoritative STOP
 * action synchronously (or throw); only after it returns do we commit the
 * finalized->stopped registry transition.
 *
 * This is a synchronous native source slice, NOT native guest await lowering.
 */
inline std::shared_ptr<SupervisorProcess> install_source_supervisor_stop_verified(
        std::string_view source,
        std::uint64_t supervisor_id,
        std::uint64_t runtime_id,
        std::size_t capacity,
        SupervisorProcess::Enqueue enqueue,
        std::shared_ptr<SupervisorChildRegistry> children,
        SupervisorProcess::DecisionSink decision_sink) {
    const oreslang::StopOnlySupervisor plan =
        oreslang::parse_stop_only_supervisor(source);
    if (plan.name.empty() || !children || !decision_sink) {
        throw std::invalid_argument("missing verified supervisor STOP authority");
    }
    // Fail at construction, not after a terminal actor PANIC has been
    // admitted, if caller wired the wrong supervisor/domain registry.
    if (children->supervisor_id() != supervisor_id
            || children->runtime_id() != runtime_id) {
        throw std::invalid_argument(
            "source STOP registry belongs to another supervisor or runtime");
    }
    auto verify = [children, supervisor_id, runtime_id](
                      const SupervisorFailureNotice& notice) {
        if (notice.supervisor_id != supervisor_id
            || notice.runtime_id != runtime_id || !notice.actor_id
            || !notice.generation || !notice.logical_child_id
            || !notice.logical_incarnation
            || notice.native_terminal_kind != ORES_NATIVE_CONTROL_PANIC) {
            throw std::invalid_argument("invalid finalized native Failure STOP target");
        }
        const auto child = SupervisorChildRegistry::ChildRef{
            supervisor_id, runtime_id, notice.logical_child_id
        };
        const auto snapshot = children->snapshot(child);
        if (snapshot.phase != SupervisorChildRegistry::Phase::finalized
            || snapshot.incarnation != notice.logical_incarnation
            || snapshot.actor.scheduler_domain != notice.runtime_id
            || snapshot.actor.actor_slot != notice.actor_id
            || snapshot.actor.generation != notice.generation) {
            throw std::logic_error("stale or unfinalized native STOP incarnation");
        }
    };
    return std::make_shared<SupervisorProcess>(
        supervisor_id, runtime_id, capacity, std::move(enqueue),
        [verify](const SupervisorFailureNotice& notice)
                -> std::optional<SupervisorProcess::Directive> {
            verify(notice);
            return SupervisorProcess::Directive::stop;
        },
        [verify, children, supervisor_id, runtime_id,
         sink = std::move(decision_sink)](
                const SupervisorProcess::Resolution& resolution) {
            if (resolution.requested != SupervisorProcess::Directive::stop) {
                throw std::logic_error("invalid verified source STOP directive");
            }
            // Recheck after policy dispatch, then claim STOP atomically
            // against concurrent RESTART *before* any external host effect.
            // If the sink throws, the effect could have partially happened:
            // retain STOPPING, never release the generation for retry.
            verify(resolution.notice);
            const auto& n = resolution.notice;
            const auto child = SupervisorChildRegistry::ChildRef{
                supervisor_id, runtime_id, n.logical_child_id};
            const auto actor = OresNativeActorRef{
                runtime_id, n.actor_id, n.generation};
            children->begin_stop(
                child, actor, n.logical_incarnation, resolution.turn_generation);
            try {
                sink(resolution);
            } catch (...) {
                // A throwing host effect may already have applied STOP.
                // Never erase the reservation or retry its side effect.
                try {
                    children->record_stop_uncertain(
                        child, actor, n.logical_incarnation, resolution.turn_generation);
                } catch (...) {
                    // Preserve the original host failure for CONTROL accounting.
                }
                throw;
            }
            // The sink's normal return is the authoritative host receipt.
            // No registry lock is held across this external callback.
            children->record_stop_applied(
                child, actor, n.logical_incarnation, resolution.turn_generation);
            children->commit_stop(
                child, actor, n.logical_incarnation, resolution.turn_generation);
        });
}

} // namespace oreslang::runtime
