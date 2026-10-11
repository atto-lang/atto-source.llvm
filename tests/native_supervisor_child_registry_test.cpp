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
    must_reject([&] {
        registry.replace(first.child, first_actor, 1,
                         OresNativeActorRef{42, 9, 1});
    });
    must_reject([&] { registry.register_child(first_actor); });
    must_reject([&] {
        registry.finalized(first.child, OresNativeActorRef{42, 0, 2}, 1);
    });
    must_reject([&] {
        registry.snapshot(SupervisorChildRegistry::ChildRef{
            9007, 42, first.child.logical_child_id});
    });

    registry.finalized(first.child, first_actor, 1);
    must_reject([&] { registry.replace(first.child, first_actor, 1, first_actor); });
    const auto next_actor = OresNativeActorRef{42, 5, 1};
    auto second = registry.replace(first.child, first_actor, 1, next_actor);
    check(second.child == first.child && second.incarnation == 2,
          "logical child identity did not survive native replacement");
    check(second.actor.actor_slot == 5 && second.actor.generation == 1,
          "new native actor incarnation was lost");
    must_reject([&] { registry.finalized(first.child, first_actor, 1); });
    must_reject([&] { registry.stop(first.child, first_actor, 1); });

    registry.finalized(first.child, next_actor, 2);
    registry.stop(first.child, next_actor, 2);
    must_reject([&] {
        registry.replace(first.child, next_actor, 2,
                         OresNativeActorRef{42, 6, 1});
    });
    registry.retire_stopped(first.child);
    check(registry.tracked_children() == 0,
          "stopped native child record leaked");
    must_reject([&] { registry.snapshot(first.child); });

    // Exactly one contender may claim a finalized generation. No duplicate
    // decision may manufacture two successor incarnations.
    const auto before_actor = OresNativeActorRef{42, 7, 1};
    const auto before = registry.register_child(before_actor);
    registry.finalized(before.child, before_actor, 1);
    std::atomic<int> wins{0};
    std::atomic<int> rejects{0};
    std::vector<std::thread> contenders;
    for (std::uint64_t i = 0; i < 2; ++i) {
        contenders.emplace_back([&, i] {
            try {
                registry.replace(before.child, before_actor, 1,
                                 OresNativeActorRef{42, 8 + i, 1});
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

    std::cout << "NATIVE_SUPERVISOR_CHILD_IDENTITY_PASS stable=1 "
                 "stale=0 cross_supervisor=0 replacement_race=1\n";
}
