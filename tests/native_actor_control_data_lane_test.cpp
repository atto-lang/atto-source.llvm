#include "oreslang/runtime/ActorGroupLifecycle.hpp"
#include "oreslang/runtime/BoundedActorPort.hpp"

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
            check(group.run_user_turn(a, [&] {
                entered.set_value();
                gate.wait(); // TEST ONLY; guest code cannot block a carrier
                ++a_turns;
            }) == NativeActorGroup::UserTurn::executed, "first DATA not admitted");
        });
        entered.get_future().wait();
        check(group.run_user_turn(a, [&] { ++a_turns; })
              == NativeActorGroup::UserTurn::busy,
              "same actor executed two DATA turns");
        check(group.enqueue_control(a, [&] { ++control_a; }),
              "CONTROL enqueue failed");
        check(group.run_control_turn(a) == NativeActorGroup::ControlTurn::busy,
              "same actor CONTROL overlapped active DATA");
        check(group.run_user_turn(a, [] {})
              == NativeActorGroup::UserTurn::busy,
              "reserved CONTROL did not preempt DATA");
        check(group.run_user_turn(b, [&] { ++b_turns; })
              == NativeActorGroup::UserTurn::executed,
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
        check(group.run_user_turn(a, [] {})
              == NativeActorGroup::UserTurn::busy,
              "DATA admitted ahead of reserved CONTROL");
        check(group.run_control_turn(a) == NativeActorGroup::ControlTurn::executed,
              "queued CONTROL not admitted");
        check(control_a == 1, "queued CONTROL delivered twice");
        check(group.run_user_turn(a, [&] { ++a_turns; })
              == NativeActorGroup::UserTurn::executed,
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
    check(group.run_user_turn(actor, [] {}) == NativeActorGroup::UserTurn::retired,
          "unready actor executed DATA");
    check(group.mark_ready(actor), "cannot ready actor");
    try {
        (void)group.run_user_turn(actor, [] { throw std::runtime_error("data failure"); });
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
    check(group.run_user_turn(actor, [] {}) == NativeActorGroup::UserTurn::executed,
          "exception stranded logical lane");
    check(group.finalized(actor), "exception stranded terminal acknowledgement");
    check(group.run_user_turn(actor, [] {}) == NativeActorGroup::UserTurn::retired,
          "retired actor admitted DATA");
    check(!group.enqueue_control(actor, [] {}), "retired actor admitted CONTROL");
}

static void bounded_port_admission_obeys_priority_and_release_fences() {
    NativeActorGroup group(505, 1);
    auto actor = group.adopt(505, 1, 1, [] {});
    auto& value = actor;
    oreslang::runtime::BoundedActorPort<std::unique_ptr<int>> port(1, sizeof(int));
    enum class PortTurn { idle, executed, busy, retired };
    auto get = [&] {
        bool consumed = false;
        // Acquire the current actor turn before touching the bounded port.
        // The old run_port_turn helper is no longer an admission authority.
        auto turn = group.run_user_turn(value, [&] {
            auto delivery = port.try_receive();
            if (!delivery) return;
            consumed = true;
            check(*delivery->value() == 42, "wrong owned port value");
            check(port.used_frames() == 1, "receive prematurely refunded credit");
            check(delivery->acknowledge_released(), "acknowledgement failed");
        });
        if (turn == NativeActorGroup::UserTurn::retired) return PortTurn::retired;
        if (turn == NativeActorGroup::UserTurn::busy) return PortTurn::busy;
        return consumed ? PortTurn::executed : PortTurn::idle;
    };
    check(get() == PortTurn::retired, "unready actor consumed port");
    check(group.mark_ready(actor), "port actor not ready");
    check(get() == PortTurn::idle, "empty port was not idle");
    std::unique_ptr<int> payload = std::make_unique<int>(42);
    check(port.offer(std::move(payload), sizeof(int)), "owned message not admitted");
    check(group.enqueue_control(actor, [] {}), "CONTROL was not reserved");
    check(get() == PortTurn::busy,
          "CONTROL priority failed to block DATA receive");
    check(port.used_frames() == 1, "busy actor consumed queued payload");
    check(group.run_control_turn(actor) == NativeActorGroup::ControlTurn::executed,
          "CONTROL did not execute");
    check(get() == PortTurn::executed,
          "queued port frame not processed after CONTROL");
    check(port.used_frames() == 0, "owned credits not released");
    check(get() == PortTurn::idle, "empty port not reported idle");
    check(group.finalized(actor), "port actor not finalized");
    port.close();
}

int main() {
    independent_actors_progress_while_first_lane_is_busy();
    exceptions_retire_lanes_and_stale_refs_fail_closed();
    bounded_port_admission_obeys_priority_and_release_fences();
}
