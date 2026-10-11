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
                registry.replace(claim, OresNativeActorRef{42, 8 + i, 1});
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
    auto committed = registry.commit_stop(stop_child.child, stop_actor, 1, 44);
    check(committed.phase == SupervisorChildRegistry::Phase::stopped,
          "authoritative STOP acknowledgment failed");
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

    std::cout << "NATIVE_SUPERVISOR_CHILD_IDENTITY_PASS stable=1 "
                 "stale=0 cross_supervisor=0 replacement_race=1\n";
}
