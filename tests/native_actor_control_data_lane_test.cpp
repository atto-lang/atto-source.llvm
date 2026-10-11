#include "oreslang/runtime/ActorGroupLifecycle.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>
#include <thread>

using oreslang::runtime::NativeActorGroup;
using namespace std::chrono_literals;

static void check(bool value, const char* detail) {
    if (!value) throw std::runtime_error(detail);
}

static void independent_actors_progress_while_first_lane_is_busy() {
    for (int cycle = 0; cycle < 120; ++cycle) {
        NativeActorGroup group(503, 2);
        auto a = group.adopt(503, 1, static_cast<std::uint64_t>(cycle + 1), []{});
        auto b = group.adopt(503, 2, static_cast<std::uint64_t>(cycle + 1), []{});
        check(group.mark_ready(a) && group.mark_ready(b), "actors not ready");
        std::promise<void> entered, release;
        auto gate = release.get_future().share();
        std::atomic<int> a_turns{0}, b_turns{0}, control_a{0}, control_b{0};

        std::thread carrier([&] {
            check(group.run_data_turn(a, [&] {
                entered.set_value();
                gate.wait(); // TEST ONLY; guest code cannot block a carrier
                ++a_turns;
            }) == NativeActorGroup::DataTurn::executed, "first DATA not admitted");
        });
        entered.get_future().wait();
        check(group.run_data_turn(a, [&] { ++a_turns; })
              == NativeActorGroup::DataTurn::busy,
              "same actor executed two DATA turns");
        check(group.enqueue_control(a, [&] { ++control_a; }),
              "CONTROL enqueue failed");
        check(group.run_control_turn(a) == NativeActorGroup::ControlTurn::busy,
              "same actor CONTROL overlapped active DATA");
        check(group.run_data_turn(a, [] {})
              == NativeActorGroup::DataTurn::busy,
              "reserved CONTROL did not preempt DATA");
        check(group.run_data_turn(b, [&] { ++b_turns; })
              == NativeActorGroup::DataTurn::executed,
              "unrelated actor blocked by first actor");
        check(group.enqueue_control(b, [&] { ++control_b; }),
              "second actor CONTROL enqueue failed");
        check(group.run_control_turn(b) == NativeActorGroup::ControlTurn::executed,
              "unrelated actor CONTROL blocked by first actor");
        check(b_turns == 1 && control_b == 1, "independent actor turns missing");
        check(!group.finalized(a), "active DATA actor physically finalized");
        release.set_value();
        carrier.join();
        check(a_turns == 1, "first DATA turn completed twice");
        check(group.run_data_turn(a, [] {})
              == NativeActorGroup::DataTurn::busy,
              "DATA admitted ahead of reserved CONTROL");
        check(group.run_control_turn(a) == NativeActorGroup::ControlTurn::executed,
              "queued CONTROL not admitted");
        check(control_a == 1, "queued CONTROL delivered twice");
        check(group.run_data_turn(a, [&] { ++a_turns; })
              == NativeActorGroup::DataTurn::executed,
              "DATA did not resume after CONTROL");
        check(a_turns == 2, "DATA resume failed");
        check(group.finalized(a) && group.finalized(b), "actors not finalized");
        a.get_done().get();
        b.get_done().get();
    }
}

static void exceptions_retire_lanes_and_stale_refs_fail_closed() {
    NativeActorGroup group(504, 1);
    auto actor = group.adopt(504, 9, 1, [] {});
    check(group.run_data_turn(actor, [] {}) == NativeActorGroup::DataTurn::retired,
          "unready actor executed DATA");
    check(group.mark_ready(actor), "cannot ready actor");
    try {
        (void)group.run_data_turn(actor, [] { throw std::runtime_error("data failure"); });
        throw std::logic_error("DATA exception was swallowed");
    } catch (const std::runtime_error& error) {
        check(std::string(error.what()) == "data failure", "DATA exception altered");
    }
    check(group.enqueue_control(actor, [] { throw std::runtime_error("panic"); }),
          "cannot queue CONTROL");
    try {
        (void)group.run_control_turn(actor);
        throw std::logic_error("CONTROL exception swallowed");
    } catch (const std::runtime_error& error) {
        check(std::string(error.what()) == "panic", "CONTROL exception altered");
    }
    check(group.run_data_turn(actor, [] {}) == NativeActorGroup::DataTurn::executed,
          "exception stranded logical lane");
    check(group.finalized(actor), "exception stranded terminal acknowledgement");
    check(group.run_data_turn(actor, [] {}) == NativeActorGroup::DataTurn::retired,
          "retired actor admitted DATA");
    check(!group.enqueue_control(actor, [] {}), "retired actor admitted CONTROL");
}

int main() {
    independent_actors_progress_while_first_lane_is_busy();
    exceptions_retire_lanes_and_stale_refs_fail_closed();
}
