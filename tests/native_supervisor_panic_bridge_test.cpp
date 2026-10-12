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

/* Test-only no-op callbacks for a scheduler whose parent CONTROL is closed. */
static void unused_message(void*, size_t, std::uint64_t) {}
static void unused_failure(void*, size_t, OresNativeActorTerminalFailure) {}

struct State {
    OresNativeActorScheduler* scheduler = nullptr;
    OresNativeActorRef parent{}, child{}, sibling{};
    std::shared_ptr<SupervisorControlScheduler> control;
    std::shared_ptr<SupervisorProcess> process;
    std::shared_ptr<oreslang::runtime::SupervisorChildRegistry> children;
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
    std::atomic<std::uint64_t> committed_generation{0};
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
    // Physical child shutdown does NOT authorize normal-exit retirement
    // once this terminal failure has been published to parent CONTROL.
    if (!s.bridge || s.bridge->retire_normally_finalized_child(failure.child))
        ++s.unexpected;
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
    s.children = std::make_shared<oreslang::runtime::SupervisorChildRegistry>(
        1107, s.parent.scheduler_domain);
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
                    || !n.logical_child_id || n.logical_incarnation != 1
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
            if (result.requested != SupervisorProcess::Directive::stop
                    || result.turn_generation == 0)
                ++s.unexpected;
            s.committed_generation.store(result.turn_generation);
            try {
                auto claim = s.bridge->reserve_finalized_stop(result);
                if (claim.phase !=
                        oreslang::runtime::SupervisorChildRegistry::Phase::stopping
                    || claim.child.logical_child_id != result.notice.logical_child_id)
                    ++s.unexpected;
            } catch (...) {
                ++s.unexpected;
            }
            // The sibling shutdown is a separate host lifecycle effect,
            // not proof that the already finalized failed child was stopped.
            if (!ores_native_actor_scheduler_stop(s.scheduler, s.sibling))
                ++s.unexpected;
            // A host lifecycle STOP can close its own supervisor from inside
            // the admitted CONTROL decision. The effect must still commit.
            s.process->close();
            try {
                auto forged = result;
                forged.notice.actor_id = s.sibling.actor_slot;
                bool rejected = false;
                try {
                    (void) s.bridge->acknowledge_finalized_stop(forged);
                } catch (const std::logic_error&) {
                    rejected = true;
                }
                if (!rejected) ++s.unexpected;
                // A forged cancellation/normal STOP terminal may never
                // acknowledge the PANIC-only source STOP disposition.
                forged = result;
                forged.notice.native_terminal_kind = ORES_NATIVE_CONTROL_CANCELLED;
                rejected = false;
                try {
                    (void) s.bridge->acknowledge_finalized_stop(forged);
                } catch (const std::logic_error&) {
                    rejected = true;
                }
                if (!rejected) ++s.unexpected;
                forged = result;
                forged.notice.native_terminal_kind = ORES_NATIVE_CONTROL_STOPPED;
                rejected = false;
                try {
                    (void) s.bridge->acknowledge_finalized_stop(forged);
                } catch (const std::logic_error&) {
                    rejected = true;
                }
                if (!rejected) ++s.unexpected;
                auto stopped = s.bridge->acknowledge_finalized_stop(result);
                if (stopped.phase !=
                        oreslang::runtime::SupervisorChildRegistry::Phase::stopped
                    || stopped.actor.actor_slot != s.child.actor_slot
                    || stopped.actor.generation != s.child.generation
                    || stopped.incarnation != result.notice.logical_incarnation
                    || stopped.child.logical_child_id != result.notice.logical_child_id)
                    ++s.unexpected;
                rejected = false;
                try {
                    (void) s.bridge->acknowledge_finalized_stop(result);
                } catch (const std::logic_error&) {
                    rejected = true;
                }
                if (!rejected) ++s.unexpected;
            } catch (...) {
                ++s.unexpected;
            }
            ++s.decisions;
            {
                std::lock_guard<std::mutex> guard(s.mutex);
                s.decision_done = true;
            }
            s.wake.notify_all();
        });

    s.bridge = std::make_unique<NativeSupervisorPanicBridge>(
        s.scheduler, s.parent, s.process, s.children);
    check(s.bridge->trusted_child_registry() == s.children,
          "panic bridge and verified lifecycle policy do not share registry");
    {
        bool rejected = false;
        try {
            auto foreign = std::make_shared<oreslang::runtime::SupervisorChildRegistry>(
                9999, s.parent.scheduler_domain);
            NativeSupervisorPanicBridge invalid{
                s.scheduler, s.parent, s.process, foreign};
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        check(rejected, "panic bridge accepted a foreign-owner child registry");
    }
    s.bridge->register_child(s.child);
    s.bridge->register_child(s.sibling); // This child exits normally via STOP.
    check(s.bridge->registered_children() == 2,
          "child generations were not tracked for terminal notification");
    check(s.children->tracked_children() == 2,
          "shared host registry did not receive bridge child admissions");
    check(!s.bridge->retire_normally_finalized_child(s.sibling),
          "active child registration retired before physical finalization");
    check(s.bridge->registered_children() == 2,
          "premature normal-exit retirement mutated registrations");
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
    // Callback entry is not the same as a completed suspension. resume()
    // deliberately rejects early completion until the policy relinquishes
    // its CONTROL carrier; avoid a scheduler-dependent false timeout here.
    const auto suspend_deadline = std::chrono::steady_clock::now() + 5s;
    while (!s.process->suspended()
            && std::chrono::steady_clock::now() < suspend_deadline)
        std::this_thread::yield();
    check(s.process->suspended(), "native guest policy never suspended");
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

    check(s.committed_generation.load() == epoch,
          "native STOP committed with a stale or missing turn generation");
    check(s.unexpected.load() == 0, "native panic bridge invariant violated");
    check(s.control_events.load() == 1 && s.policy_calls.load() == 1
          && s.decisions.load() == 1,
          "terminal failure delivered or resolved more than once");
    check(s.bridge->registered_children() == 1,
          "terminal child replay registration was not consumed");
    check(s.bridge->accepted() == 1 && s.bridge->rejected() == 3,
          "native supervisor bridge replay/forgery accounting drift");
    check(!ores_native_actor_scheduler_publish_failure(
                  s.scheduler, s.parent, replay),
          "native scheduler published a duplicate terminal failure generation");
    check(s.process->resolved() == 1 && !s.process->healthy()
          && s.process->undelivered() == 0,
          "self-closing supervisor lost or duplicated a committed decision");
    check(ores_native_actor_scheduler_poll_done(s.scheduler, s.child)
          && ores_native_actor_scheduler_poll_done(s.scheduler, s.sibling),
          "finalized child or stopped sibling is still executing");
    check(!s.bridge->retire_normally_finalized_child(s.child),
          "failed child's terminal registration was reused");
    auto wrong_generation = s.sibling;
    ++wrong_generation.generation;
    check(!s.bridge->retire_normally_finalized_child(wrong_generation),
          "stale or future generation retired a different child's registration");
    check(s.bridge->retire_normally_finalized_child(s.sibling),
          "normally finalized sibling did not retire its registration");
    OresNativeActorTerminalFailure suppressed{
        ORES_NATIVE_CONTROL_PANIC, s.sibling, 99, 123
    };
    check(!ores_native_actor_scheduler_publish_failure(
                  s.scheduler, s.parent, suppressed),
          "clean-exit seal permitted a late terminal failure");
    check(!s.bridge->retire_normally_finalized_child(s.sibling),
          "normal child registration was retired twice");
    check(s.bridge->registered_children() == 0,
          "normally finalized child left an orphan registration");
    check(s.bridge->tracked_logical_children() == 1,
          "normal child did not retire or terminal child lost its STOP record");

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

    // A lifecycle effect that closes its own supervisor and then throws
    // has unknown commit status. It must not be replayed or marked resolved;
    // the deferred close must still finish and account for the lost notice.
    {
        auto control = std::make_shared<SupervisorControlScheduler>(1, 8);
        std::shared_ptr<SupervisorProcess> throwing;
        std::atomic<unsigned> attempts{0};
        throwing = std::make_shared<SupervisorProcess>(
            2207, 51, 2, control->adapter(),
            [](const SupervisorFailureNotice&)
                    -> std::optional<SupervisorProcess::Directive> {
                return SupervisorProcess::Directive::stop;
            },
            [&](const SupervisorProcess::Resolution&) {
                ++attempts;
                throwing->close();
                throw std::runtime_error("ambiguous lifecycle side effect");
            });
        throwing->accept(SupervisorFailureNotice{
            2207, 51, 1, 1, std::nullopt, "panic"});
        auto until = std::chrono::steady_clock::now() + 5s;
        while (throwing->undelivered() == 0
               && std::chrono::steady_clock::now() < until)
            std::this_thread::yield();
        check(attempts.load() == 1, "throwing lifecycle sink retried");
        check(throwing->resolved() == 0,
              "throwing lifecycle sink was falsely committed");
        check(throwing->undelivered() == 1 && throwing->pending() == 0,
              "self-close failed to account for and clear abandoned notice");
        check(!throwing->callback_active() && !throwing->healthy(),
              "self-closing failed supervisor retained an active lease");
        bool rejected = false;
        try {
            throwing->accept(SupervisorFailureNotice{
                2207, 51, 2, 1, std::nullopt, "late"});
        } catch (const std::logic_error&) {
            rejected = true;
        }
        check(rejected, "late CONTROL was accepted after failing self-close");
        throwing->close();
        check(throwing->undelivered() == 2,
              "closing twice double-counted an already-accounted notice");
        control->close();
    }

    // A decision callback may request self-close before it returns.
    // That request must fence *new* CONTROL admission immediately, even
    // though the current authorized effect has not committed yet.
    {
        auto control = std::make_shared<SupervisorControlScheduler>(1, 8);
        std::shared_ptr<SupervisorProcess> process;
        std::atomic<unsigned> rejected{0};
        process = std::make_shared<SupervisorProcess>(
            5507, 55, 2, control->adapter(),
            [](const SupervisorFailureNotice&)
                    -> std::optional<SupervisorProcess::Directive> {
                return SupervisorProcess::Directive::stop;
            },
            [&](const SupervisorProcess::Resolution&) {
                process->close();
                if (process->healthy()) {
                    throw std::runtime_error("self-close still reports healthy");
                }
                try {
                    process->accept(SupervisorFailureNotice{
                        5507, 55, 2, 1, std::nullopt, "late after close"});
                } catch (const std::logic_error&) {
                    ++rejected;
                }
            });
        process->accept(SupervisorFailureNotice{
            5507, 55, 1, 1, std::nullopt, "first"});
        auto until = std::chrono::steady_clock::now() + 5s;
        while (process->resolved() == 0
                && std::chrono::steady_clock::now() < until)
            std::this_thread::yield();
        check(rejected.load() == 1 && process->resolved() == 1,
              "native reentrant close admitted another CONTROL notice");
        check(process->undelivered() == 1 && process->pending() == 0,
              "native close request lost rejected CONTROL accounting");
        control->close();
    }

    // Terminal policy failure must release its lease and discard retained
    // owned notice data without making an ambiguous lifecycle decision.
    {
        auto control = std::make_shared<SupervisorControlScheduler>(1, 8);
        auto process = std::make_shared<SupervisorProcess>(
            3307, 53, 2, control->adapter(),
            [](const SupervisorFailureNotice&)
                    -> std::optional<SupervisorProcess::Directive> {
                throw std::runtime_error("policy failed");
            },
            [](const SupervisorProcess::Resolution&) {
                throw std::runtime_error("failed policy reached lifecycle sink");
            });
        process->accept(SupervisorFailureNotice{
            3307, 53, 1, 1, std::nullopt, "policy failure"});
        auto until = std::chrono::steady_clock::now() + 5s;
        while (process->undelivered() == 0
                && std::chrono::steady_clock::now() < until)
            std::this_thread::yield();
        check(process->undelivered() == 1 && process->pending() == 0,
              "native terminal failure retained a CONTROL notice");
        check(!process->callback_active() && !process->healthy(),
              "native terminal failure retained logical lease");
        process->close();
        check(process->undelivered() == 1,
              "native close double-counted failed notification");
        control->close();
    }

    // Closing a suspended hook invalidates its logical lease immediately.
    {
        auto control = std::make_shared<SupervisorControlScheduler>(1, 8);
        std::atomic<unsigned> entered{0};
        auto process = std::make_shared<SupervisorProcess>(
            4407, 54, 2, control->adapter(),
            [&](const SupervisorFailureNotice&)
                    -> std::optional<SupervisorProcess::Directive> {
                ++entered;
                return std::nullopt;
            },
            [](const SupervisorProcess::Resolution&) {
                throw std::runtime_error("closed supervisor reached lifecycle sink");
            });
        process->accept(SupervisorFailureNotice{
            4407, 54, 1, 1, std::nullopt, "suspend"});
        auto until = std::chrono::steady_clock::now() + 5s;
        while (entered.load() == 0 && std::chrono::steady_clock::now() < until)
            std::this_thread::yield();
        check(entered.load() == 1 && process->callback_active(),
              "suspended native callback never acquired its logical lease");
        const auto suspend_deadline = std::chrono::steady_clock::now() + 5s;
        while (!process->suspended()
                && std::chrono::steady_clock::now() < suspend_deadline)
            std::this_thread::yield();
        check(process->suspended(), "native close test never reached suspension");
        const auto epoch = process->current_epoch();
        check(epoch != 0, "suspended native callback lost generation");
        process->close();
        check(!process->callback_active() && process->pending() == 0,
              "native close retained suspended logical lease or notice");
        check(process->undelivered() == 1 && !process->healthy(),
              "native close misaccounted abandoned continuation");
        check(!process->resume(epoch, SupervisorProcess::Directive::restart),
              "stale native callback resumed after close");
        control->close();
    }


    // A thrown host CONTROL callback must retire every queued owned notice
    // without replaying the handler or leaving a phantom pending event.
    {
        auto control = std::make_shared<SupervisorControlScheduler>(1, 8);
        auto lane = std::make_shared<oreslang::runtime::SupervisorFailureControl>(
            3307, 3);
        std::mutex gate_mutex;
        std::condition_variable gate;
        bool entered = false;
        bool release = false;
        std::atomic<unsigned> calls{0};
        lane->install(
            [&](const SupervisorFailureNotice&) {
                ++calls;
                std::unique_lock<std::mutex> lock(gate_mutex);
                entered = true;
                gate.notify_all();
                if (!gate.wait_for(lock, 5s, [&] { return release; }))
                    throw std::runtime_error("CONTROL test timed out");
                throw std::runtime_error("CONTROL callback failed");
            },
            control->adapter());
        lane->publish(SupervisorFailureNotice{
            3307, 53, 1, 1, std::nullopt, "first failure"});
        {
            std::unique_lock<std::mutex> lock(gate_mutex);
            check(gate.wait_for(lock, 5s, [&] { return entered; }),
                  "CONTROL callback never started");
        }
        lane->publish(SupervisorFailureNotice{
            3307, 53, 2, 1, std::nullopt, "pending failure"});
        check(lane->pending() == 2, "queued CONTROL notices were not retained");
        {
            std::lock_guard<std::mutex> lock(gate_mutex);
            release = true;
        }
        gate.notify_all();
        const auto until = std::chrono::steady_clock::now() + 5s;
        while (lane->healthy() && std::chrono::steady_clock::now() < until)
            std::this_thread::yield();
        check(calls.load() == 1, "failed CONTROL callback was replayed");
        check(lane->undelivered() == 2 && lane->pending() == 0,
              "failed CONTROL callback retained queued notice payloads");
        lane->publish(SupervisorFailureNotice{
            3307, 53, 3, 1, std::nullopt, "late failure"});
        check(lane->undelivered() == 3 && lane->pending() == 0,
              "late failure was accepted onto a permanently failed lane");
        control->close();
    }


    // A direct/synchronous adapter is forbidden for CONTROL admission.
    // Reject the callback before execution and discard its owned envelope.
    {
        auto lane = std::make_shared<oreslang::runtime::SupervisorFailureControl>(
            4407, 1);
        std::atomic<unsigned> called{0};
        lane->install(
            [&](const SupervisorFailureNotice&) { ++called; },
            [](std::function<void()> fn) {
                fn(); // Illegal inline attempt: enqueue guard suppresses work.
                return true;
            });
        lane->publish(SupervisorFailureNotice{
            4407, 54, 1, 1, std::nullopt, "inline not allowed"});
        check(called.load() == 0, "inline CONTROL callback executed");
        check(!lane->healthy() && lane->undelivered() == 1
              && lane->pending() == 0,
              "inline CONTROL rejection retained a pending notice");
    }


    // A process itself must also fail admission if the enqueue adapter
    // rejected the CONTROL turn. No caller may mistake that for success.
    {
        std::atomic<unsigned> policies{0};
        auto process = std::make_shared<SupervisorProcess>(
            5507, 55, 2,
            [](std::function<void()> fn) {
                fn(); // Illegal same-thread invocation must be suppressed.
                return true;
            },
            [&](const SupervisorFailureNotice&)
                    -> std::optional<SupervisorProcess::Directive> {
                ++policies;
                return SupervisorProcess::Directive::stop;
            },
            [](const SupervisorProcess::Resolution&) {
                throw std::runtime_error("inline process committed a lifecycle effect");
            });
        bool rejected = false;
        try {
            process->accept(SupervisorFailureNotice{
                5507, 55, 1, 1, std::nullopt, "direct process"});
        } catch (const std::logic_error&) {
            rejected = true;
        }
        check(rejected && policies.load() == 0,
              "failed CONTROL process dispatch was reported as accepted");
        check(process->undelivered() == 1 && process->pending() == 0
              && !process->callback_active() && !process->healthy(),
              "rejected CONTROL process dispatch retained its notice or lease");
    }

    // Even an unadmitted terminal failure remains classified as a failure:
    // parent shutdown is not permission to erase the child's registration.
    {
        auto* abandoned = ores_native_actor_scheduler_open_typed_control(
            2, 1, 2, unused_message, nullptr, unused_failure, nullptr);
        check(abandoned != nullptr, "abandoned CONTROL scheduler startup failed");
        auto parent = ores_native_actor_scheduler_spawn(abandoned, 0);
        auto child = ores_native_actor_scheduler_spawn(abandoned, 1);
        check(parent.scheduler_domain && child.scheduler_domain,
              "abandoned CONTROL actors failed to spawn");
        check(ores_native_actor_scheduler_stop(abandoned, child),
              "terminal child did not stop");
        check(ores_native_actor_scheduler_wait_done(abandoned, child),
              "terminal child was not physically finalized");
        check(ores_native_actor_scheduler_stop(abandoned, parent),
              "parent did not retire");
        check(!ores_native_actor_scheduler_publish_failure(
                      abandoned, parent,
                      OresNativeActorTerminalFailure{
                          ORES_NATIVE_CONTROL_PANIC, child, 77, 99
                      }),
              "terminal event was delivered to a closed parent");
        check(!ores_native_actor_scheduler_seal_normal_exit(abandoned, child),
              "rejected panic was reclassified as a normal exit");
        ores_native_actor_scheduler_retire(abandoned);
        check(ores_native_actor_scheduler_drain(abandoned),
              "abandoned CONTROL scheduler did not drain");
        check(ores_native_actor_scheduler_try_destroy(abandoned),
              "abandoned CONTROL scheduler teardown failed");
    }

    // True scheduler-backed RESTART admission and shutdown fencing. The
    // replacement must be physically READY, not just a fresh actor number.
    for (int mode = 0; mode < 3; ++mode) {
        auto* runtime = ores_native_actor_scheduler_open_typed_control(
            3, 1, 2, unused_message, nullptr, unused_failure, nullptr);
        check(runtime != nullptr, "restart proof scheduler startup failed");
        const auto supervisor = ores_native_actor_scheduler_spawn(runtime, 0);
        const auto dying = ores_native_actor_scheduler_spawn(runtime, 1);
        const auto replacement = ores_native_actor_scheduler_spawn_unpublished(runtime, 2);
        check(supervisor.scheduler_domain && dying.scheduler_domain
                  && replacement.scheduler_domain, "restart fixture spawn failed");
        auto control = std::make_shared<SupervisorControlScheduler>(2, 16);
        auto children = std::make_shared<oreslang::runtime::SupervisorChildRegistry>(
            9107, supervisor.scheduler_domain);
        auto process = std::make_shared<SupervisorProcess>(
            9107, supervisor.scheduler_domain, 2, control->adapter(),
            [](const SupervisorFailureNotice&)
                    -> std::optional<SupervisorProcess::Directive> {
                return std::nullopt;
            },
            [](const SupervisorProcess::Resolution&) {});
        auto bridge = std::make_unique<NativeSupervisorPanicBridge>(
            runtime, supervisor, process, children);
        const auto child = children->register_child(dying);
        check(ores_native_actor_scheduler_stop(runtime, dying)
                  && ores_native_actor_scheduler_wait_done(runtime, dying),
              "old actor did not physically finalize before restart");
        children->finalized(child.child, dying, 1);
        const auto ticket = children->begin_restart(child.child, dying, 1);
        check(!ores_native_actor_scheduler_send(runtime, replacement, 42)
                  && !ores_native_actor_scheduler_submit(
                      runtime, static_cast<size_t>(replacement.actor_slot), 42),
              "unpublished replacement accepted mail before logical commit");
        bool rejected = false;
        try {
            (void) bridge->admit_verified_replacement(
                ticket, OresNativeActorRef{
                    supervisor.scheduler_domain, replacement.actor_slot,
                    replacement.generation + 1});
        } catch (const std::exception&) { rejected = true; }
        check(rejected, "bridge accepted forged scheduler generation as READY");
        if (mode == 0) {
            const auto committed = bridge->admit_verified_replacement(
                ticket, replacement);
            check(ores_native_actor_scheduler_send(runtime, replacement, 42),
                  "logically committed replacement remains unroutable");
            check(committed.phase ==
                      oreslang::runtime::SupervisorChildRegistry::Phase::active
                      && committed.incarnation == 2,
                  "native scheduler-backed RESTART failed to publish next incarnation");
            check(bridge->registered_children() == 1,
                  "replacement was published without its panic-routing index");
            check(ores_native_actor_scheduler_stop(runtime, replacement)
                      && ores_native_actor_scheduler_wait_done(runtime, replacement),
                  "replacement did not physically finalize after commit");
            check(bridge->on_panic(supervisor.actor_slot,
                      OresNativeActorTerminalFailure{
                          ORES_NATIVE_CONTROL_PANIC, replacement, 19, 219}),
                  "new generation was missing from supervisor PANIC routing");
            bridge->fence_restart_publication();
        } else if (mode == 1) {
            children->stage_replacement(ticket, replacement);
            rejected = false;
            try { (void) children->abort_restart(ticket); }
            catch (const std::exception&) { rejected = true; }
            check(rejected, "live actor restart was aborted without cleanup");
            check(ores_native_actor_scheduler_stop(runtime, replacement)
                      && ores_native_actor_scheduler_wait_done(runtime, replacement),
                  "replacement did not physically terminate");
            const auto restored = bridge->abort_cleaned_replacement(ticket, replacement);
            check(restored.phase ==
                      oreslang::runtime::SupervisorChildRegistry::Phase::finalized,
                  "native restart cleanup did not restore finalized old child");
            bridge->fence_restart_publication();
            rejected = false;
            try { (void) children->begin_restart(child.child, dying, 1); }
            catch (const std::exception&) { rejected = true; }
            check(rejected, "closed bridge reopened restart admission");
        } else {
            // Closing CONTROL while the bridge remains alive must fence the
            // replacement even though the physical scheduler actor is READY.
            process->close();
            rejected = false;
            try { (void) bridge->admit_verified_replacement(ticket, replacement); }
            catch (const std::logic_error&) { rejected = true; }
            check(rejected, "closed supervisor published a READY replacement");
            check(children->snapshot(child.child).phase
                      == oreslang::runtime::SupervisorChildRegistry::Phase::restarting,
                  "closed CONTROL changed pending RESTART reservation");
            check(bridge->registered_children() == 0,
                  "failed closed admission registered an uncommitted actor");
            check(ores_native_actor_scheduler_stop(runtime, replacement)
                      && ores_native_actor_scheduler_wait_done(runtime, replacement),
                  "rejected replacement did not physically clean up");
            check(children->abort_restart(ticket).phase
                      == oreslang::runtime::SupervisorChildRegistry::Phase::finalized,
                  "closed supervisor could not release unspawned claim");
        }
        bridge.reset();
        process->close();
        control->close();
        ores_native_actor_scheduler_retire(runtime);
        check(ores_native_actor_scheduler_drain(runtime)
                  && ores_native_actor_scheduler_try_destroy(runtime),
              "native restart proof scheduler teardown failed");
    }

    std::puts("NATIVE_SUPERVISOR_BRIDGE_PASS typed=1 exclusive=1"
              " await=1 replay=0 native_stop=1 throwing_self_close=1"
              " terminal_cleanup=1 suspended_close=1");
}
