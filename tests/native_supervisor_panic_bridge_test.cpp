#include "oreslang/runtime/NativeActorScheduler.h"
#include "oreslang/runtime/NativeSupervisorPanicBridge.hpp"
#include "oreslang/runtime/SupervisorControlScheduler.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;
using oreslang::runtime::NativeSupervisorPanicBridge;
using oreslang::runtime::SupervisorControlScheduler;
using oreslang::runtime::SupervisorFailureNotice;
using oreslang::runtime::SupervisorProcess;

static void check(bool okay, const char* reason) {
    if (!okay) throw std::runtime_error(reason);
}

struct State {
    OresNativeActorScheduler* scheduler = nullptr;
    OresNativeActorRef parent{}, child{}, sibling{};
    std::shared_ptr<SupervisorControlScheduler> control;
    std::shared_ptr<SupervisorProcess> process;
    std::unique_ptr<NativeSupervisorPanicBridge> bridge;
    std::mutex mutex;
    std::condition_variable wake;
    bool child_cleanup = false;
    bool child_published_while_active = false;
    bool release_child_carrier = false;
    bool callback_entered = false;
    bool decision_done = false;
    std::atomic<unsigned> unexpected{0};
    std::atomic<unsigned> control_events{0};
    std::atomic<unsigned> policy_calls{0};
    std::atomic<unsigned> decisions{0};
    std::thread::id child_carrier;
    std::thread::id policy_carrier;
};

static void ordinary_message(void* raw, size_t actor, std::uint64_t payload) {
    auto& s = *static_cast<State*>(raw);
    if (actor != 1 || payload != 99) {
        ++s.unexpected;
        return;
    }
    s.child_carrier = std::this_thread::get_id();
    // Resource cleanup must run before the typed panic is enqueued.
    {
        std::lock_guard<std::mutex> guard(s.mutex);
        s.child_cleanup = true;
    }
    if (!ores_native_actor_scheduler_stop(s.scheduler, s.child))
        ++s.unexpected;
    OresNativeActorTerminalFailure failure{
        ORES_NATIVE_CONTROL_PANIC, s.child, 37, 20261011
    };
    if (!ores_native_actor_scheduler_publish_failure(
            s.scheduler, s.parent, failure))
        ++s.unexpected;
    {
        std::unique_lock<std::mutex> lock(s.mutex);
        s.child_published_while_active = true;
        s.wake.notify_all();
        // With multiple native workers the parent CONTROL may be schedulable,
        // but must wait for this actual child turn to finish physically.
        if (!s.wake.wait_for(lock, 5s,
                [&] { return s.release_child_carrier; }))
            ++s.unexpected;
    }
}

static void typed_parent_control(
        void* raw, size_t parent_actor,
        OresNativeActorTerminalFailure failure) {
    auto& s = *static_cast<State*>(raw);
    ++s.control_events;
    if (!s.bridge || !s.bridge->on_panic(parent_actor, failure))
        ++s.unexpected;
}

int main() {
    State s;
    // Native actor scheduler reserves one exclusive CONTROL slot for parent.
    // Its typed callback forwards into a *separate* host supervisor scheduler.
    s.scheduler = ores_native_actor_scheduler_open_typed_control(
        3, 1, 3, ordinary_message, nullptr, typed_parent_control, &s);
    check(s.scheduler != nullptr, "native actor scheduler startup failed");
    s.parent = ores_native_actor_scheduler_spawn(s.scheduler, 0);
    s.child = ores_native_actor_scheduler_spawn(s.scheduler, 1);
    s.sibling = ores_native_actor_scheduler_spawn(s.scheduler, 2);
    check(s.parent.scheduler_domain && s.child.scheduler_domain
          && s.sibling.scheduler_domain, "native actor spawn failed");

    s.control = std::make_shared<SupervisorControlScheduler>(2, 32);
    s.process = std::make_shared<SupervisorProcess>(
        1107, s.parent.scheduler_domain, 8, s.control->adapter(),
        [&](const SupervisorFailureNotice& n)
                -> std::optional<SupervisorProcess::Directive> {
            ++s.policy_calls;
            s.policy_carrier = std::this_thread::get_id();
            if (n.supervisor_id != 1107
                    || n.runtime_id != s.parent.scheduler_domain
                    || n.actor_id != s.child.actor_slot
                    || n.generation != s.child.generation
                    || n.native_terminal_kind != ORES_NATIVE_CONTROL_PANIC
                    || n.failure_code != 37 || n.correlation != 20261011
                    || !s.child_cleanup)
                ++s.unexpected;
            {
                std::lock_guard<std::mutex> guard(s.mutex);
                s.callback_entered = true;
            }
            s.wake.notify_all();
            // This is an await-like suspension: keep the logical supervisor
            // lease while freeing the physical CONTROL carrier.
            return std::nullopt;
        },
        [&](const SupervisorProcess::Resolution& result) {
            if (result.requested != SupervisorProcess::Directive::stop)
                ++s.unexpected;
            if (!ores_native_actor_scheduler_stop(s.scheduler, s.sibling))
                ++s.unexpected;
            // A real lifecycle STOP can close its own supervisor from inside
            // the admitted CONTROL decision. The effect must still commit.
            s.process->close();
            ++s.decisions;
            {
                std::lock_guard<std::mutex> guard(s.mutex);
                s.decision_done = true;
            }
            s.wake.notify_all();
        });

    s.bridge = std::make_unique<NativeSupervisorPanicBridge>(
        s.scheduler, s.parent, s.process);
    s.bridge->register_child(s.child);
    OresNativeActorTerminalFailure premature{
        ORES_NATIVE_CONTROL_PANIC, s.child, 37, 20261011
    };
    check(!ores_native_actor_scheduler_publish_failure(
            s.scheduler, s.parent, premature),
          "active child could publish a terminal failure before stop");
    check(ores_native_actor_scheduler_send(s.scheduler, s.child, 99),
          "child terminal message not admitted");

    {
        std::unique_lock<std::mutex> lock(s.mutex);
        check(s.wake.wait_for(lock, 5s,
                [&] { return s.child_published_while_active; }),
              "native child never published its typed terminal event");
        check(s.control_events.load() == 0 && s.policy_calls.load() == 0,
              "parent CONTROL started while failed child was still executing");
        s.release_child_carrier = true;
    }
    s.wake.notify_all();
    {
        std::unique_lock<std::mutex> lock(s.mutex);
        check(s.wake.wait_for(lock, 5s, [&] { return s.callback_entered; }),
              "typed native failure did not reach supervisor CONTROL policy");
    }
    check(s.policy_carrier != s.child_carrier,
          "supervisor guest policy ran on child actor's carrier");
    check(s.process->callback_active() && s.process->resolved() == 0,
          "supervisor await lost logical single-flight lease");
    const std::uint64_t epoch = s.process->current_epoch();
    check(epoch != 0, "supervisor await generation missing");
    OresNativeActorTerminalFailure forged{
        ORES_NATIVE_CONTROL_PANIC, s.sibling, 71, 9
    };
    check(!s.bridge->on_panic(0, forged),
          "unregistered sibling spoofed a panic event");
    OresNativeActorTerminalFailure replay{
        ORES_NATIVE_CONTROL_PANIC, s.child, 37, 20261011
    };
    check(!s.bridge->on_panic(0, replay),
          "duplicate terminal failure reached same supervisor generation");
    auto stale = replay;
    ++stale.child.generation;
    check(!s.bridge->on_panic(0, stale),
          "stale child generation impersonated current actor");

    std::thread foreign_completion([&] {
        if (!s.process->resume(epoch, SupervisorProcess::Directive::stop))
            ++s.unexpected;
    });
    foreign_completion.join();
    check(!s.process->resume(epoch, SupervisorProcess::Directive::restart),
          "duplicate await completion admitted after first resume");
    {
        std::unique_lock<std::mutex> lock(s.mutex);
        check(s.wake.wait_for(lock, 5s, [&] { return s.decision_done; }),
              "supervisor decision never applied native sibling stop");
    }

    auto deadline = std::chrono::steady_clock::now() + 5s;
    while (s.process->resolved() != 1 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();

    check(s.unexpected.load() == 0, "native panic bridge invariant violated");
    check(s.control_events.load() == 1 && s.policy_calls.load() == 1
          && s.decisions.load() == 1,
          "terminal failure delivered or resolved more than once");
    check(s.bridge->accepted() == 1 && s.bridge->rejected() == 3,
          "native supervisor bridge replay/forgery accounting drift");
    check(s.process->resolved() == 1 && !s.process->healthy()
          && s.process->undelivered() == 0,
          "self-closing supervisor lost or duplicated a committed decision");
    check(ores_native_actor_scheduler_poll_done(s.scheduler, s.child)
          && ores_native_actor_scheduler_poll_done(s.scheduler, s.sibling),
          "finalized child or stopped sibling is still executing");

    check(ores_native_actor_scheduler_stop(s.scheduler, s.parent),
          "parent supervisor lane could not stop");
    ores_native_actor_scheduler_retire(s.scheduler);
    check(ores_native_actor_scheduler_drain(s.scheduler),
          "native scheduler could not drain terminal CONTROL");
    check(ores_native_actor_scheduler_control_completed(s.scheduler) == 1,
          "native scheduler lost or duplicated reserved typed CONTROL");
    check(ores_native_actor_scheduler_try_destroy(s.scheduler),
          "native scheduler teardown failed");
    s.scheduler = nullptr;
    s.bridge.reset();
    s.process->close();
    s.control->close();

    std::puts("NATIVE_SUPERVISOR_BRIDGE_PASS typed=1 exclusive=1"
              " await=1 replay=0 native_stop=1");
}
