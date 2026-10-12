#include "oreslang/runtime/SupervisorChildRegistry.hpp"

#include <atomic>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

using oreslang::runtime::SupervisorChildRegistry;

static void check(bool value, const char* reason) {
    if (!value) throw std::runtime_error(reason);
}
template <class F> static void must_reject(F&& fn) {
    try { fn(); } catch (const std::exception&) { return; }
    throw std::runtime_error("unsafe supervisor child transition was accepted");
}

int main() {
    SupervisorChildRegistry registry{1107, 42};
    // Native actor slots are zero-based; logical generations are separate
    // from scheduler (slot, generation) incarnation identities.
    OresNativeActorRef first_actor{42, 0, 1};
    auto first = registry.register_child(first_actor);
    check(first.incarnation == 1
          && first.phase == SupervisorChildRegistry::Phase::active,
          "initial logical child incarnation missing");
    must_reject([&] { registry.begin_restart(first.child, first_actor, 1); });
    must_reject([&] { registry.register_child(first_actor); });
    must_reject([&] {
        registry.finalized(first.child, OresNativeActorRef{42, 0, 2}, 1);
    });
    must_reject([&] {
        registry.snapshot(SupervisorChildRegistry::ChildRef{
            9007, 42, first.child.logical_child_id});
    });

    // Recreating a registry for the same supervisor/domain must NOT recycle
    // logical child 1: a stale handle must not address a new child (ABA).
    SupervisorChildRegistry another_registry{1107, 42};
    const auto other_registry_child = another_registry.register_child(
        OresNativeActorRef{42, 4, 1});
    check(other_registry_child.child.logical_child_id
              != first.child.logical_child_id,
          "reinitialized registry reused stale logical child ID");
    must_reject([&] { registry.snapshot(other_registry_child.child); });
    must_reject([&] { another_registry.snapshot(first.child); });

    registry.finalized(first.child, first_actor, 1);
    must_reject([&] { registry.register_child(first_actor); });
    auto reservation = registry.begin_restart(first.child, first_actor, 1);
    check(registry.snapshot(first.child).phase ==
              SupervisorChildRegistry::Phase::restarting,
          "begin_restart did not acquire exclusive lifecycle reservation");
    must_reject([&] { registry.begin_restart(first.child, first_actor, 1); });
    must_reject([&] { registry.stop(first.child, first_actor, 1); });
    must_reject([&] { registry.replace(reservation, first_actor); });
    const auto next_actor = OresNativeActorRef{42, 5, 1};
    must_reject([&] { registry.replace(reservation, next_actor); });
    registry.stage_replacement(reservation, next_actor);
    must_reject([&] { registry.replace(reservation, next_actor); });
    must_reject([&] {
        registry.acknowledge_replacement_admission(
            reservation, next_actor, [] { return false; });
    });
    registry.acknowledge_replacement_admission(
        reservation, next_actor, [] { return true; });
    auto second = registry.replace(reservation, next_actor);
    must_reject([&] { registry.replace(reservation, OresNativeActorRef{42, 9, 1}); });
    check(second.child == first.child && second.incarnation == 2,
          "logical child identity did not survive native replacement");
    check(second.actor.actor_slot == 5 && second.actor.generation == 1,
          "new native actor incarnation was lost");
    must_reject([&] { registry.finalized(first.child, first_actor, 1); });
    must_reject([&] { registry.stop(first.child, first_actor, 1); });

    registry.finalized(first.child, next_actor, 2);
    const auto reused = registry.begin_restart(first.child, next_actor, 2);
    must_reject([&] { registry.replace(reused, first_actor); });
    // Slot 5 can only be reincarnated with a higher scheduler generation;
    // an older incarnation must not be revived by a logical RESTART.
    must_reject([&] {
        registry.replace(reused, OresNativeActorRef{42, 5, 1});
    });
    registry.abort_restart(reused);
    registry.stop(first.child, next_actor, 2);
    must_reject([&] { registry.begin_restart(first.child, next_actor, 2); });
    registry.retire_stopped(first.child);
    check(registry.tracked_children() == 0,
          "stopped native child record leaked");
    must_reject([&] { registry.snapshot(first.child); });

    // Exactly one contender may claim a finalized generation. No duplicate
    // decision may manufacture two successor incarnations.
    const auto before_actor = OresNativeActorRef{42, 7, 1};
    const auto before = registry.register_child(before_actor);
    registry.finalized(before.child, before_actor, 1);
    // A failed spawn releases only the holder's permit. A stale permit
    // cannot cancel or commit a newer reservation for the same generation.
    const auto aborted = registry.begin_restart(before.child, before_actor, 1);
    registry.abort_restart(aborted);
    const auto current = registry.begin_restart(before.child, before_actor, 1);
    check(current.nonce != aborted.nonce, "restart permit nonce was reused");
    must_reject([&] { registry.abort_restart(aborted); });
    must_reject([&] { registry.replace(aborted, OresNativeActorRef{42, 10, 1}); });
    registry.abort_restart(current);
    std::atomic<int> wins{0};
    std::atomic<int> rejects{0};
    std::vector<std::thread> contenders;
    for (std::uint64_t i = 0; i < 2; ++i) {
        contenders.emplace_back([&, i] {
            try {
                auto claim = registry.begin_restart(before.child, before_actor, 1);
                const auto candidate = OresNativeActorRef{42, 8 + i, 1};
                registry.stage_replacement(claim, candidate);
                registry.acknowledge_replacement_admission(
                    claim, candidate, [] { return true; });
                registry.replace(claim, candidate);
                ++wins;
            } catch (const std::exception&) {
                ++rejects;
            }
        });
    }
    for (auto& contender : contenders) contender.join();
    check(wins == 1 && rejects == 1,
          "native child replacement was not exclusive");
    check(registry.snapshot(before.child).incarnation == 2,
          "native logical generation fence failed");
    // Even with a fresh logical child ID, the same native slot must not
    // host two active generations, and a completed generation is not reusable.
    const auto live = registry.snapshot(before.child).actor;
    must_reject([&] {
        registry.register_child(OresNativeActorRef{
            42, live.actor_slot, live.generation + 1});
    });
    registry.finalized(before.child, live, 2);
    must_reject([&] { registry.register_child(live); });
    const auto newer = registry.register_child(
        OresNativeActorRef{42, live.actor_slot, live.generation + 1});
    check(newer.incarnation == 1 && newer.child.logical_child_id
              != before.child.logical_child_id,
          "new scheduler generation was not admitted independently");

    // Admission rollback retires an ACTIVE child without pretending its
    // physical actor finalized. Preserve the scheduler-generation fence.
    const auto attempted_actor = OresNativeActorRef{42, 90, 1};
    const auto attempted = registry.register_child(attempted_actor);
    must_reject([&] {
        registry.detach_active(attempted.child, OresNativeActorRef{42, 90, 2}, 1);
    });
    registry.detach_active(attempted.child, attempted_actor, 1);
    must_reject([&] { registry.snapshot(attempted.child); });
    must_reject([&] { registry.register_child(attempted_actor); });
    const auto fresh = registry.register_child(OresNativeActorRef{42, 90, 2});
    check(fresh.child.logical_child_id != attempted.child.logical_child_id,
          "admission rollback recycled logical child ID");

    // STOP and RESTART are competing terminal claims on one finalized
    // generation. The STOP turn remains exclusive across external awaits.
    const OresNativeActorRef stop_actor{42, 91, 1};
    const auto stop_child = registry.register_child(stop_actor);
    registry.finalized(stop_child.child, stop_actor, 1);
    auto pending_stop = registry.begin_stop(stop_child.child, stop_actor, 1, 44);
    check(pending_stop.phase == SupervisorChildRegistry::Phase::stopping,
          "STOP did not hold a logical lifecycle reservation");
    must_reject([&] { registry.begin_restart(stop_child.child, stop_actor, 1); });
    must_reject([&] { registry.begin_stop(stop_child.child, stop_actor, 1, 45); });
    must_reject([&] {
        registry.commit_stop(stop_child.child, stop_actor, 1, 45);
    });
    must_reject([&] {
        registry.commit_stop(stop_child.child, OresNativeActorRef{42, 91, 2}, 1, 44);
    });
    check(registry.snapshot(stop_child.child).phase
                  == SupervisorChildRegistry::Phase::stopping,
          "forged STOP acknowledgment released a lifecycle reservation");
    using Receipt = SupervisorChildRegistry::StopReceiptStatus;
    check(registry.stop_receipt(stop_child.child).status == Receipt::pending,
          "STOP reservation did not create PENDING host receipt");
    must_reject([&] { registry.commit_stop(stop_child.child, stop_actor, 1, 44); });
    // The synchronous host sink succeeded; its exact-turn receipt can
    // transition PENDING to APPLIED before committing STOPPED.
    check(registry.stop_receipt(stop_child.child).status == Receipt::pending,
          "native STOP callback was unexpectedly reclassified");
    must_reject([&] { registry.begin_restart(stop_child.child, stop_actor, 1); });
    must_reject([&] {
        registry.record_stop_applied(stop_child.child, stop_actor, 1, 45);
    });
    registry.record_stop_applied(stop_child.child, stop_actor, 1, 44);
    check(registry.stop_receipt(stop_child.child).status == Receipt::applied,
          "host proof did not advance exact reserved STOP receipt");
    auto committed = registry.commit_stop(stop_child.child, stop_actor, 1, 44);
    check(committed.phase == SupervisorChildRegistry::Phase::stopped,
          "authoritative STOP acknowledgment failed");
    check(registry.stop_receipt(stop_child.child).status == Receipt::acknowledged,
          "committed STOP lacks ACKNOWLEDGED host receipt");
    must_reject([&] { registry.commit_stop(stop_child.child, stop_actor, 1, 44); });
    must_reject([&] { registry.begin_restart(stop_child.child, stop_actor, 1); });

    const OresNativeActorRef racing_actor{42, 92, 1};
    const auto racing = registry.register_child(racing_actor);
    registry.finalized(racing.child, racing_actor, 1);
    wins = 0;
    rejects = 0;
    std::thread stop_contender([&] {
        try {
            registry.begin_stop(racing.child, racing_actor, 1, 46);
            ++wins;
        } catch (const std::exception&) { ++rejects; }
    });
    std::thread restart_contender([&] {
        try {
            registry.begin_restart(racing.child, racing_actor, 1);
            ++wins;
        } catch (const std::exception&) { ++rejects; }
    });
    stop_contender.join();
    restart_contender.join();
    check(wins == 1 && rejects == 1,
          "STOP and RESTART simultaneously claimed the same native generation");

    // A separate unresolved host STOP can be reconciled only by a
    // trusted, independently checked outcome for the same CONTROL turn.
    const OresNativeActorRef uncertain_actor{42, 93, 1};
    const auto uncertain_child = registry.register_child(uncertain_actor);
    registry.finalized(uncertain_child.child, uncertain_actor, 1);
    registry.begin_stop(uncertain_child.child, uncertain_actor, 1, 72);
    registry.record_stop_uncertain(uncertain_child.child, uncertain_actor, 1, 72);
    must_reject([&] {
        registry.record_stop_applied(uncertain_child.child, uncertain_actor, 1, 72);
    });
    must_reject([&] {
        registry.commit_stop(uncertain_child.child, uncertain_actor, 1, 72);
    });
    check(registry.snapshot(uncertain_child.child).phase
                  == SupervisorChildRegistry::Phase::stopping,
          "unverified UNCERTAIN receipt left quarantine");
    unsigned proof_calls = 0;
    const auto no_proof = [&](const SupervisorChildRegistry::StopReceipt& receipt) {
        ++proof_calls;
        return receipt.turn_generation == 0; // Host reports UNKNOWN.
    };
    check(!registry.reconcile_uncertain_stop(
                uncertain_child.child, uncertain_actor, 1, 72, no_proof),
          "UNKNOWN reconciliation was treated as committed STOP");
    must_reject([&] {
        registry.reconcile_uncertain_stop(
            uncertain_child.child, uncertain_actor, 1, 72,
            [](const SupervisorChildRegistry::StopReceipt&) -> bool {
                throw std::runtime_error("host receipt verification unavailable");
            });
    });
    check(registry.snapshot(uncertain_child.child).phase
                  == SupervisorChildRegistry::Phase::stopping
              && registry.stop_receipt(uncertain_child.child).status
                  == SupervisorChildRegistry::StopReceiptStatus::uncertain,
          "throwing independent verifier altered UNKNOWN lifecycle state");

    check(registry.snapshot(uncertain_child.child).phase
                  == SupervisorChildRegistry::Phase::stopping
              && registry.stop_receipt(uncertain_child.child).status
                  == SupervisorChildRegistry::StopReceiptStatus::uncertain,
          "unverified STOP reopened a quarantined generation");
    must_reject([&] {
        registry.reconcile_uncertain_stop(uncertain_child.child, uncertain_actor,
            1, 73, [&](const SupervisorChildRegistry::StopReceipt&) {
                ++proof_calls; return true;
            });
    });
    check(proof_calls == 1, "forged turn invoked external STOP verifier");
    const auto accepted_proof = [&](const SupervisorChildRegistry::StopReceipt& receipt) {
        ++proof_calls;
        // The external verifier must never run while holding the registry
        // lock. Reentrant introspection would deadlock otherwise.
        check(registry.snapshot(uncertain_child.child).phase
                    == SupervisorChildRegistry::Phase::stopping,
              "STOP verifier executed under a blocked registry mutex");
        check(registry.stop_receipt(uncertain_child.child).status
                    == SupervisorChildRegistry::StopReceiptStatus::uncertain,
              "STOP verifier observed a fabricated applied receipt");
        return receipt.child == uncertain_child.child
            && receipt.failed_actor.generation == uncertain_actor.generation
            && receipt.incarnation == 1 && receipt.turn_generation == 72;
    };
    check(registry.reconcile_uncertain_stop(
                uncertain_child.child, uncertain_actor, 1, 72, accepted_proof),
          "independently acknowledged STOP did not reconcile");
    check(registry.snapshot(uncertain_child.child).phase
                  == SupervisorChildRegistry::Phase::stopped
              && registry.stop_receipt(uncertain_child.child).status
                  == SupervisorChildRegistry::StopReceiptStatus::acknowledged,
          "reconciled native STOP lost terminal receipt");
    must_reject([&] {
        registry.reconcile_uncertain_stop(
            uncertain_child.child, uncertain_actor, 1, 72, accepted_proof);
    });

    // A process can close after reserving STOP but before its host
    // callback records UNCERTAIN. PENDING must remain quarantined; only an
    // independent verifier can close that same original CONTROL turn.
    const OresNativeActorRef pending_actor{42, 94, 1};
    const auto pending_child = registry.register_child(pending_actor);
    registry.finalized(pending_child.child, pending_actor, 1);
    registry.begin_stop(pending_child.child, pending_actor, 1, 80);
    must_reject([&] {
        registry.reconcile_uncertain_stop(
            pending_child.child, pending_actor, 1, 80,
            [](const SupervisorChildRegistry::StopReceipt&) { return true; });
    });
    must_reject([&] {
        registry.reconcile_unresolved_stop(
            pending_child.child, pending_actor, 1, 81,
            [](const SupervisorChildRegistry::StopReceipt&) { return true; });
    });
    check(!registry.reconcile_unresolved_stop(
                pending_child.child, pending_actor, 1, 80,
                [](const SupervisorChildRegistry::StopReceipt&) { return false; }),
          "unverified PENDING host effect was committed");
    check(registry.snapshot(pending_child.child).phase
                  == SupervisorChildRegistry::Phase::stopping,
          "unverified PENDING receipt escaped STOP quarantine");
    check(registry.reconcile_unresolved_stop(
                pending_child.child, pending_actor, 1, 80,
                [&](const SupervisorChildRegistry::StopReceipt& candidate) {
                    check(registry.stop_receipt(candidate.child).status
                                == SupervisorChildRegistry::StopReceiptStatus::pending,
                          "PENDING verifier blocked registry inspection");
                    return candidate.child == pending_child.child
                        && candidate.turn_generation == 80
                        && candidate.incarnation == 1;
                }),
          "independent proof could not reconcile PENDING shutdown STOP");
    check(registry.snapshot(pending_child.child).phase
                  == SupervisorChildRegistry::Phase::stopped
              && registry.stop_receipt(pending_child.child).status
                  == SupervisorChildRegistry::StopReceiptStatus::acknowledged,
          "PENDING proof did not commit the same terminal STOP turn");
    must_reject([&] {
        registry.reconcile_unresolved_stop(
            pending_child.child, pending_actor, 1, 80,
            [](const SupervisorChildRegistry::StopReceipt&) { return true; });
    });

    // Physical RESTART candidates are tracked separately from logical child
    // identity. No commit without admission and no abort while still alive.
    SupervisorChildRegistry gated{2217, 42};
    const OresNativeActorRef failed{42, 110, 1};
    const auto target = gated.register_child(failed);
    gated.finalized(target.child, failed, 1);
    const auto ticket = gated.begin_restart(target.child, failed, 1);
    const OresNativeActorRef staged{42, 111, 1};
    must_reject([&] { gated.replace(ticket, staged); });
    gated.stage_replacement(ticket, staged);
    must_reject([&] { gated.abort_restart(ticket); });
    must_reject([&] { gated.replace(ticket, staged); });
    must_reject([&] {
        gated.acknowledge_replacement_admission(
            ticket, staged, [] { return false; });
    });
    gated.acknowledge_replacement_admission(
        ticket, staged, [&] {
            check(gated.snapshot(target.child).phase
                      == SupervisorChildRegistry::Phase::restarting,
                  "admission callback was invoked holding registry mutex");
            return true;
        });
    must_reject([&] { gated.abort_restart(ticket); });
    gated.close_restart_admission();
    must_reject([&] { gated.replace(ticket, staged); });
    must_reject([&] {
        gated.acknowledge_replacement_cleanup(
            ticket, staged, [] { return false; });
    });
    gated.acknowledge_replacement_cleanup(
        ticket, staged, [&] {
            check(gated.snapshot(target.child).phase
                      == SupervisorChildRegistry::Phase::restarting,
                  "cleanup callback was invoked holding registry mutex");
            return true;
        });
    must_reject([&] { gated.replace(ticket, staged); });
    gated.abort_restart(ticket);
    check(gated.snapshot(target.child).phase
              == SupervisorChildRegistry::Phase::finalized,
          "cleanup-backed restart rollback did not restore FINALIZED");
    must_reject([&] { gated.begin_restart(target.child, failed, 1); });

    // Shared 11-action model-refinement trace. Actual scheduler readiness
    // is checked separately in native_supervisor_panic_bridge_test.cpp.
    {
        SupervisorChildRegistry trace_registry{3317, 42};
        const OresNativeActorRef old{42, 130, 1};
        const auto child = trace_registry.register_child(old);
        const auto first = [&] {
            trace_registry.finalized(child.child, old, 1);
            return trace_registry.begin_restart(child.child, old, 1);
        }();
        const OresNativeActorRef new_actor{42, 131, 1};
        trace_registry.stage_replacement(first, new_actor);
        must_reject([&] { trace_registry.replace(first, new_actor); });
        trace_registry.acknowledge_replacement_admission(
            first, new_actor, [] { return true; });
        const auto activated = trace_registry.replace(first, new_actor);
        check(activated.incarnation == 2
                  && activated.child == child.child,
              "shared trace failed to preserve logical identity");
        trace_registry.finalized(activated.child, new_actor, 2);
        const auto retry = trace_registry.begin_restart(activated.child, new_actor, 2);
        const OresNativeActorRef candidate{42, 132, 1};
        trace_registry.stage_replacement(retry, candidate);
        must_reject([&] { trace_registry.abort_restart(retry); });
        trace_registry.acknowledge_replacement_cleanup(
            retry, candidate, [] { return true; });
        check(trace_registry.abort_restart(retry).phase
                  == SupervisorChildRegistry::Phase::finalized,
              "shared trace failed to restore original generation");
        std::cout
            << "SUPERVISOR_RESTART_TRACE_V1="
            << "ACTIVE>FINALIZED>RESTARTING>SPAWNED>ADMITTED>"
               "ACTIVE>FINALIZED>RESTARTING>SPAWNED>CLEANED>FINALIZED\n";
    }

    // The physical scheduler slot is exclusive while a RESTART candidate is
    // merely staged; the active-only index is insufficient at that point.
    {
        SupervisorChildRegistry exclusive{4417, 42};
        const OresNativeActorRef dying_a{42, 150, 1};
        const OresNativeActorRef dying_b{42, 151, 1};
        const auto a = exclusive.register_child(dying_a);
        const auto b = exclusive.register_child(dying_b);
        exclusive.finalized(a.child, dying_a, 1);
        exclusive.finalized(b.child, dying_b, 1);
        const auto claim_a = exclusive.begin_restart(a.child, dying_a, 1);
        const auto claim_b = exclusive.begin_restart(b.child, dying_b, 1);
        const OresNativeActorRef candidate{42, 152, 1};
        exclusive.stage_replacement(claim_a, candidate);
        must_reject([&] { exclusive.stage_replacement(claim_b, candidate); });
        must_reject([&] {
            exclusive.stage_replacement(claim_b, OresNativeActorRef{42, 152, 2});
        });
        must_reject([&] { exclusive.register_child(candidate); });
        exclusive.acknowledge_replacement_admission(
            claim_a, candidate, [] { return true; });
        exclusive.replace(claim_a, candidate);
        must_reject([&] { exclusive.stage_replacement(claim_b, candidate); });
        exclusive.abort_restart(claim_b);
    }

    // Staging consumes a physical scheduler generation even if the actor
    // is cleaned and its uncommitted RESTART reservation is aborted. Otherwise
    // stale callbacks could address a reused slot+generation (ABA).
    {
        SupervisorChildRegistry abandoned{5517, 42};
        const OresNativeActorRef old{42, 160, 1};
        const OresNativeActorRef spawned{42, 161, 1};
        const auto child = abandoned.register_child(old);
        abandoned.finalized(child.child, old, 1);
        auto claim = abandoned.begin_restart(child.child, old, 1);
        abandoned.stage_replacement(claim, spawned);
        abandoned.acknowledge_replacement_cleanup(
            claim, spawned, [] { return true; });
        abandoned.abort_restart(claim);
        must_reject([&] { abandoned.register_child(spawned); });
        const auto renewed = abandoned.begin_restart(child.child, old, 1);
        must_reject([&] { abandoned.stage_replacement(renewed, spawned); });
        abandoned.abort_restart(renewed);
        // A genuinely new physical scheduler generation may still enter.
        const auto newer = abandoned.register_child(
            OresNativeActorRef{42, spawned.actor_slot, spawned.generation + 1});
        check(newer.incarnation == 1 && newer.child != child.child,
              "fenced physical generation blocked a genuinely new incarnation");
    }


    // The journal below is emitted from real committed registry transitions,
    // not the older hand-assembled phase-only trace. It intentionally does
    // not assert unobserved CONTROL ownership or host STOP invocation.
    {
        SupervisorChildRegistry observed{6617, 42};
        const OresNativeActorRef old{42, 171, 1};
        const auto child = observed.register_child(old);
        observed.finalized(child.child, old, 1);
        const auto permit = observed.begin_restart(child.child, old, 1);
        const OresNativeActorRef next{42, 172, 1};
        observed.stage_replacement(permit, next);
        must_reject([&] { observed.replace(permit, next); });
        observed.acknowledge_replacement_admission(
            permit, next, [] { return true; });
        auto activated = observed.replace(permit, next);
        observed.finalized(activated.child, next, 2);
        const auto retry = observed.begin_restart(activated.child, next, 2);
        const OresNativeActorRef doomed{42, 173, 1};
        observed.stage_replacement(retry, doomed);
        must_reject([&] { observed.abort_restart(retry); });
        observed.acknowledge_replacement_cleanup(
            retry, doomed, [] { return true; });
        observed.abort_restart(retry);
        const auto entries = observed.committed_transitions();
        const std::vector<std::string> expected{
            "register", "finalize-asserted", "restart.reserve",
            "restart.spawn-asserted", "restart.admit-verified", "restart.commit",
            "finalize-asserted", "restart.reserve", "restart.spawn-asserted",
            "restart.cleanup-verified", "restart.abort"};
        check(entries.size() == expected.size() && observed.audit_trace_complete(),
              "native registry journal was incomplete");
        for (std::size_t i = 0; i < entries.size(); ++i) {
            check(entries[i].sequence == i + 1
                  && std::string(entries[i].operation) == expected[i]
                  && entries[i].state.has_value()
                  && entries[i].state->child == child.child,
                  "native journal asserted a missing or out-of-order transition");
        }
        check(entries[2].restart_permit == permit.nonce
              && entries[5].restart_permit == permit.nonce
              && entries[7].restart_permit == retry.nonce
              && entries[10].restart_permit == retry.nonce
              && permit.nonce != retry.nonce,
              "native journal lost restart permit incarnation fencing");
        check(entries[5].state->incarnation == 2
              && entries[5].replacement.has_value(),
              "native committed restart journal lost new incarnation");
        check(entries[10].replacement.has_value()
              && entries[10].replacement->scheduler_domain == doomed.scheduler_domain
              && entries[10].replacement->actor_slot == doomed.actor_slot
              && entries[10].replacement->generation == doomed.generation,
              "cleanup-backed abort lost physical candidate identity");
        std::cout << "SUPERVISOR_REGISTRY_TRANSITIONS_V1=";
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (i) std::cout << '>';
            std::cout << entries[i].operation;
        }
        std::cout << '\n';
    }

    {
        SupervisorChildRegistry observed{6717, 42};
        const OresNativeActorRef old{42, 181, 1};
        const auto child = observed.register_child(old);
        observed.finalized(child.child, old, 1);
        observed.begin_stop(child.child, old, 1, 43);
        observed.record_stop_uncertain(child.child, old, 1, 43);
        observed.record_stop_uncertain(child.child, old, 1, 43);
        const auto entries = observed.committed_transitions();
        check(entries.size() == 4
              && std::string(entries[2].operation) == "stop.reserve"
              && std::string(entries[3].operation) == "stop.uncertain"
              && entries[3].stop_turn == 43,
              "native uncertain STOP was duplicated or falsely acknowledged");
        check(observed.snapshot(child.child).phase
              == SupervisorChildRegistry::Phase::stopping,
              "native STOP uncertainty lost its quarantine");
    }

    {
        SupervisorChildRegistry observed{6817, 42};
        const OresNativeActorRef old{42, 191, 1};
        const auto child = observed.register_child(old);
        observed.finalized(child.child, old, 1);
        const auto permit = observed.begin_restart(child.child, old, 1);
        const OresNativeActorRef next{42, 192, 1};
        observed.stage_replacement(permit, next);
        observed.acknowledge_replacement_admission(
            permit, next, [] { return true; });
        observed.close_restart_admission();
        must_reject([&] { observed.replace(permit, next); });
        must_reject([&] { observed.abort_restart(permit); });
        observed.acknowledge_replacement_cleanup(
            permit, next, [] { return true; });
        observed.abort_restart(permit);
        const auto entries = observed.committed_transitions();
        check(entries.size() == 8
              && std::string(entries[5].operation) == "restart.close"
              && std::string(entries[6].operation) == "restart.cleanup-verified"
              && std::string(entries[7].operation) == "restart.abort",
              "native journal missed close/cleanup/abort transaction");
    }

    std::cout << "NATIVE_SUPERVISOR_CHILD_IDENTITY_PASS stable=1 "
                 "stale=0 cross_supervisor=0 replacement_race=1\n";
}
