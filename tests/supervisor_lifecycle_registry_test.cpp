#include "oreslang/runtime/NativeSupervisorRegistry.hpp"
#include <atomic>
#include <iostream>
#include <optional>
#include <thread>

using F = oreslang::runtime::SupervisorLifecycleFsm;
using R = oreslang::runtime::NativeSupervisorRegistry;
using C = oreslang::SupervisorControlTurn;
using L = oreslang::SupervisorFailureLane;
#define CHECK(x) do { if (!(x)) throw std::runtime_error("failed: " #x); } while (false)
template<class Function> static void rejects(Function fn) {
    bool rejected = false;
    try { fn(); } catch (const std::logic_error&) { rejected = true; }
    CHECK(rejected);
}
static F::Event event(F::Kind kind) { return F::Event{kind, 1}; }
static F::Event child(std::uint64_t id, std::uint64_t epoch = 1) {
    auto e = event(F::Kind::child_finished); e.child = {id, epoch}; return e;
}
struct Hooks : C::Hooks {
    std::atomic<bool> entered{false}, release{false};
    bool block = false;
    unsigned restarts = 0, escalations = 0, user = 0;
    C::Decision on_throw(const L::Failure&) override { return C::Decision::Restart; }
    C::Decision on_raise(const L::Failure&) override { return C::Decision::Restart; }
    C::Decision on_panic(const L::Failure&) override { return C::Decision::Restart; }
    C::Decision on_cancel(const L::Failure&) override { return C::Decision::Restart; }
    C::Decision on_host_failure(const L::Failure&) override { return C::Decision::Restart; }
    void on_escalate(const L::Failure&) override { ++escalations; }
    bool on_restart(const L::Failure&) override {
        ++restarts;
        entered.store(true, std::memory_order_release);
        if (block) while (!release.load(std::memory_order_acquire)) std::this_thread::yield();
        return true;
    }
    void on_user_turn() override { ++user; }
};
static void lifecycle_and_detached_worker() {
    R tree(9001, 8, 8, 2);
    auto root = tree.root(); auto middle = tree.create_child(root);
    auto leaf = tree.create_child(middle); auto sibling = tree.create_child(root);
    auto group = tree.create_group(leaf, 2);
    unsigned stops = 0;
    auto actor = group->adopt(9001, 1, 7, [&] { ++stops; });
    CHECK(group->mark_ready(actor));
    auto finalizer = group->runtime_finalizer(actor);
    auto snapshot = tree.status();
    CHECK(!snapshot.admissions_fenced && snapshot.nodes.size() == 4);
    CHECK(tree.begin_shutdown());
    CHECK(tree.begin_shutdown());
    CHECK(snapshot.nodes.front().lifecycle.admissions_open()); // old snapshot stays unchanged
    rejects([&] { (void)tree.create_child(root); });
    rejects([&] { (void)tree.create_group(root, 1); });
    rejects([&] { (void)group->adopt(9001, 2, 1, [] {}); });
    CHECK(stops == 0 && !group->closed()); // fencing does not cancel accepted actors
    rejects([&] { tree.observe(leaf, event(F::Kind::before_finished)); });
    tree.observe(root, event(F::Kind::before_finished));
    tree.observe(middle, event(F::Kind::before_finished));
    tree.observe(leaf, event(F::Kind::before_finished));
    rejects([&] { tree.observe(root, child(middle.id())); });
    rejects([&] { tree.observe(leaf, event(F::Kind::actors_finished)); });
    group->close(); CHECK(stops == 1);
    CHECK(group->worker_count() == 0);
    group.reset(); // weak registry reference must not hide still-live detached actor
    rejects([&] { tree.observe(leaf, event(F::Kind::actors_finished)); });
    auto deadline = event(F::Kind::deadline); tree.observe(root, deadline);
    CHECK(tree.status().has_deadlines && !tree.status().all_logically_stopped);
    CHECK(finalizer.finalized());
    CHECK(!finalizer.finalized());
    tree.observe(leaf, event(F::Kind::actors_finished));
    auto failure = event(F::Kind::after_finished); failure.failed = true;
    tree.observe(leaf, failure);
    auto stale = child(leaf.id(), 0);
    tree.observe(middle, stale);
    rejects([&] { tree.observe(middle, child(leaf.id(), 2)); });
    rejects([&] { tree.observe(root, child(leaf.id())); });
    tree.observe(middle, child(leaf.id()));
    tree.observe(middle, child(leaf.id())); // duplicate is inert
    tree.observe(middle, event(F::Kind::actors_finished));
    tree.observe(middle, event(F::Kind::after_finished));
    tree.observe(root, child(middle.id()));
    tree.observe(sibling, event(F::Kind::before_finished));
    tree.observe(sibling, event(F::Kind::actors_finished));
    tree.observe(sibling, event(F::Kind::after_finished));
    tree.observe(root, child(sibling.id()));
    tree.observe(root, event(F::Kind::actors_finished));
    tree.observe(root, event(F::Kind::after_finished));
    const auto stopped = tree.status();
    CHECK(stopped.all_logically_stopped && !stopped.all_physically_reclaimed);
    CHECK(stopped.has_failures && stopped.has_deadlines);
    rejects([&] { tree.observe(root, event(F::Kind::physical_release)); });
    R foreign(9001); rejects([&] { tree.observe(foreign.root(), deadline); });
    auto stale_parent = deadline; stale_parent.epoch = 0;
    tree.observe(root, stale_parent);
    CHECK(tree.status().nodes.front().lifecycle.revision == stopped.nodes.front().lifecycle.revision);
}
static void stop_restart_race() {
    for (unsigned iteration = 0; iteration < 100; ++iteration) {
        R tree(10000 + iteration, 4, 4, 2);
        auto root = tree.root(); auto group = tree.create_group(root, 1);
        auto lane = tree.control_lane(root);
        Hooks hooks; hooks.block = true;
        L::ActorKey old{"old", 1}, late{"late", 2};
        lane->reserve_child(old); lane->reserve_child(late);
        CHECK(lane->publish_child(old, L::Kind::Panic, "panic"));
        std::thread turn([&] { lane->admit_turn(hooks, false); });
        while (!hooks.entered.load(std::memory_order_acquire)) std::this_thread::yield();
        // A preexisting policy turn may finish. No busy wait in begin_shutdown.
        const bool deferred = !tree.begin_shutdown();
        const auto partial = tree.status();
        bool rejected = false;
        try { (void)group->adopt(10000 + iteration, 1, 1, [] {}); }
        catch (const std::logic_error&) { rejected = true; }
        hooks.release.store(true, std::memory_order_release);
        turn.join();
        CHECK(deferred && partial.admissions_fenced && !partial.shutdown_committed && rejected);
        CHECK(tree.begin_shutdown());
        CHECK(lane->publish_child(late, L::Kind::Panic, "panic"));
        CHECK(lane->admit_turn(hooks, true) == C::Admitted::Control);
        CHECK(hooks.restarts == 1 && hooks.escalations == 1);
        CHECK(lane->restarts_used() == 1); // suppressed restart spends no budget
        CHECK(lane->admit_turn(hooks, true) == C::Admitted::None);
        CHECK(hooks.user == 0);
    }
}
static void spawn_fence_race() {
    for (unsigned iteration = 0; iteration < 100; ++iteration) {
        const auto runtime = 20000 + iteration;
        R tree(runtime, 4, 4);
        auto root = tree.root(); auto group = tree.create_group(root, 1);
        std::atomic<bool> go{false};
        std::optional<oreslang::runtime::NativeActorRef> accepted;
        bool rejected = false;
        std::thread producer([&] {
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            try { accepted = group->adopt(runtime, 1, 1, [] {}); }
            catch (const std::logic_error&) { rejected = true; }
        });
        go.store(true, std::memory_order_release);
        const bool committed = tree.begin_shutdown();
        producer.join();
        CHECK(committed && (bool(accepted) != rejected));
        tree.observe(root, event(F::Kind::before_finished));
        if (accepted) {
            CHECK(group->mark_ready(*accepted));
            auto finalizer = group->runtime_finalizer(*accepted);
            group->close();
            rejects([&] { tree.observe(root, event(F::Kind::actors_finished)); });
            CHECK(finalizer.finalized());
        }
        tree.observe(root, event(F::Kind::actors_finished));
        tree.observe(root, event(F::Kind::after_finished));
        CHECK(tree.status().all_logically_stopped);
        CHECK(!tree.status().all_physically_reclaimed);
    }
}
int main() {
    lifecycle_and_detached_worker(); stop_restart_race(); spawn_fence_race();
    std::cout << "PASS registry lifecycle, detached worker truth, 100 STOP/restart and 100 spawn/fence races\n";
}
