// Host-substrate implementation test for L01-L05 of
// contracts/v1/ACTOR_LIFECYCLE_CONFORMANCE_V1.json.
// This does NOT certify guest repeated-await lowering or send_async.
#include "oreslang/runtime/ActorGroupLifecycle.hpp"
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>

using oreslang::runtime::NativeActorGroup;
using namespace std::chrono_literals;
#define CHECK(x) do { if (!(x)) throw std::runtime_error("lifecycle check: " #x); } while (false)

template<class F> static bool fails(F&& f) {
    try { f(); return false; }
    catch (const std::exception&) { return true; }
}
static void repeated_success_observations() {
    NativeActorGroup group(8801, 1);
    auto actor = group.adopt(8801, 1, 1, [] {});
    auto finalizer = group.runtime_finalizer(actor);
    auto ready = actor.get_ready(), done = actor.get_done();
    int ready_callbacks = 0, done_callbacks = 0;
    actor.when_ready([&](const auto& result) {
        CHECK(result.ready);
        ++ready_callbacks; // host notification only; never guest execution
    });
    actor.when_ready([&](const auto& result) {
        CHECK(result.ready);
        ++ready_callbacks;
    });
    actor.when_done([&](const auto& result) {
        CHECK(result.success);
        ++done_callbacks;
    });
    CHECK(ready.wait_for(0s) == std::future_status::timeout);
    CHECK(done.wait_for(0s) == std::future_status::timeout);
    CHECK(group.mark_ready(actor));
    CHECK(ready_callbacks == 2);
    actor.when_ready([&](const auto& result) {
        CHECK(result.ready);
        ++ready_callbacks;
    });
    CHECK(ready_callbacks == 3); // late subscriber sees settled result
    ready.get();
    ready.get();
    actor.get_ready().get();
    CHECK(done.wait_for(0s) == std::future_status::timeout);
    group.stop_workers();
    CHECK(done.wait_for(0s) == std::future_status::timeout);
    group.close();
    CHECK(finalizer.ready_for_resource_reclamation());
    CHECK(finalizer.finalized());
    CHECK(done_callbacks == 1);
    actor.when_done([&](const auto& result) {
        CHECK(result.success);
        ++done_callbacks;
    });
    CHECK(done_callbacks == 2);
    done.get();
    done.get();
    actor.get_done().get();
    ready.get(); // readiness remains settled even after physical DONE
    CHECK(!group.enqueue_control(actor, [] {}));
    CHECK(group.run_user_turn(actor, [] {}) == NativeActorGroup::UserTurn::retired);
    CHECK(fails([&] { actor.retain_resource_loan(); }));
}
static void failed_startup_replayed_to_all_observers() {
    NativeActorGroup group(8802, 1);
    auto actor = group.adopt(8802, 1, 1, [] {});
    auto finalizer = group.runtime_finalizer(actor);
    auto ready = actor.get_ready(), done = actor.get_done();
    int failed_ready_callbacks = 0;
    actor.when_ready([&](const auto& result) {
        CHECK(!result.ready);
        ++failed_ready_callbacks;
    });
    actor.when_ready([&](const auto& result) {
        CHECK(!result.ready);
        ++failed_ready_callbacks;
    });
    group.stop_workers();
    CHECK(failed_ready_callbacks == 2);
    actor.when_ready([&](const auto& result) {
        CHECK(!result.ready);
        ++failed_ready_callbacks;
    });
    CHECK(failed_ready_callbacks == 3);
    CHECK(fails([&] { ready.get(); }));
    CHECK(fails([&] { ready.get(); }));
    CHECK(fails([&] { actor.get_ready().get(); }));
    CHECK(!group.mark_ready(actor));
    CHECK(done.wait_for(0s) == std::future_status::timeout);
    group.close();
    CHECK(finalizer.finalized());
    CHECK(fails([&] { done.get(); }));
    CHECK(fails([&] { done.get(); }));
    CHECK(fails([&] { actor.get_done().get(); }));
}
static void terminal_loan_and_control_fencing() {
    NativeActorGroup group(8803, 1);
    auto actor = group.adopt(8803, 1, 1, [] {});
    auto finalizer = group.runtime_finalizer(actor);
    CHECK(group.mark_ready(actor));
    actor.retain_resource_loan();
    CHECK(group.enqueue_control(actor, [] {}));
    CHECK(!group.finalized(actor));
    group.close();
    CHECK(!finalizer.ready_for_resource_reclamation());
    CHECK(!finalizer.finalized());
    actor.release_resource_loan();
    CHECK(finalizer.ready_for_resource_reclamation());
    CHECK(finalizer.finalized());
    actor.get_done().get();
    CHECK(!finalizer.finalized());
    CHECK(!group.enqueue_control(actor, [] {}));
    CHECK(fails([&] { actor.retain_resource_loan(); }));
}
static void single_flight_and_control_precedence() {
    NativeActorGroup group(8804, 1);
    auto actor = group.adopt(8804, 1, 1, [] {});
    CHECK(group.mark_ready(actor));
    std::promise<void> entered, release;
    auto entered_future = entered.get_future();
    auto release_future = release.get_future().share();
    std::thread holder([&] {
        CHECK(group.run_user_turn(actor, [&] {
            entered.set_value();
            release_future.wait(); // TEST ONLY: deterministic host-side gate
        }) == NativeActorGroup::UserTurn::executed);
    });
    if (entered_future.wait_for(5s) != std::future_status::ready) {
        release.set_value();
        holder.join();
        throw std::runtime_error("user turn admission timed out");
    }
    const auto busy = group.run_user_turn(actor, [] {});
    int control = 0;
    const auto reserved = group.enqueue_control(actor, [&] { ++control; });
    const auto blocked = group.run_control_turn(actor);
    release.set_value();
    holder.join();
    CHECK(busy == NativeActorGroup::UserTurn::busy);
    CHECK(reserved);
    CHECK(blocked == NativeActorGroup::ControlTurn::busy);
    CHECK(group.run_control_turn(actor) == NativeActorGroup::ControlTurn::executed);
    CHECK(control == 1);
    CHECK(group.finalized(actor));
    actor.get_done().get();
}
int main() {
    repeated_success_observations();
    failed_startup_replayed_to_all_observers();
    terminal_loan_and_control_fencing();
    single_flight_and_control_precedence();
    std::cout << "ACTOR_LIFECYCLE_L01_L05_HOST_PASS\n";
}
