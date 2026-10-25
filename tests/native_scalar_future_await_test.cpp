#include "oreslang/runtime/NativeScalarFutureAwait.hpp"
#include <atomic>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <thread>

using oreslang::runtime::NativeActorGroup;
using oreslang::runtime::NativeScalarFutureAwait;

static void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

int main() {
    NativeActorGroup group(9701, 2);
    auto owner = group.adopt(9701, 1, 1, [] {});
    auto foreign = group.adopt(9701, 2, 1, [] {});
    check(group.mark_ready(owner) && group.mark_ready(foreign), "admission failed");
    auto future = NativeScalarFutureAwait::open(owner, 17);
    const auto id = future->id();
    auto stale = id;
    stale.generation += 1;
    check(!future->complete(stale, 1), "stale generation settled value");
    check(!future->fail(id, 0), "invalid failure status admitted");
    check(!future->try_schedule(id, group, owner, [] (const auto&) {}),
          "pending future scheduled");
    check(group.enqueue_control(owner, [] {}), "unable to fill CONTROL");
    std::thread producer([future, id] {
        check(future->complete(id, 42), "completion lost");
        check(!future->complete(id, 43), "duplicate completion admitted");
    });
    producer.join();
    int resumed = 0;
    auto resume = [&](const NativeScalarFutureAwait::Result& value) {
        check(value.success && value.value == 42 && value.error_code == 0,
              "corrupt scalar future result");
        ++resumed;
    };
    check(!future->try_schedule(id, group, foreign, resume), "foreign CONTROL owner");
    check(!future->try_schedule(id, group, owner, resume), "full CONTROL accepted");
    check(future->phase() == NativeScalarFutureAwait::Phase::ready,
          "backpressure erased terminal value");
    check(group.run_control_turn(owner) == NativeActorGroup::ControlTurn::executed,
          "failed to drain CONTROL");
    check(future->try_schedule(id, group, owner, resume), "retry lost value");
    check(resumed == 0, "guest resumed inline on scheduling thread");
    check(group.run_control_turn(owner) == NativeActorGroup::ControlTurn::executed,
          "CONTROL did not resume");
    check(resumed == 1, "not exactly one resumption");
    check(future->phase() == NativeScalarFutureAwait::Phase::completed,
          "frame not completed");
    check(!future->try_schedule(id, group, owner, resume), "repeated resume accepted");
    check(!future->cancel(id), "completed resume cancelled");

    // Cancellation after CONTROL admission revokes queued guest invocation.
    auto queued = NativeScalarFutureAwait::open(owner, 18);
    check(queued->fail(queued->id(), 7), "failure not recorded");
    check(queued->try_schedule(queued->id(), group, owner,
          [&](const auto&) { ++resumed; }), "could not enqueue failed result");
    check(queued->cancel(queued->id()), "queued cancel rejected");
    check(group.run_control_turn(owner) == NativeActorGroup::ControlTurn::executed,
          "cancelled CONTROL not drained");
    check(resumed == 1, "cancelled guest callback executed");

    // Numeric Future IDs are routing/debug labels, not capabilities. Two
    // simultaneously live frames may accidentally reuse the same numeric ID;
    // a ticket issued by one frame must not settle, cancel, or schedule the
    // other frame.
    auto duplicate_a = NativeScalarFutureAwait::open(owner, 21);
    auto duplicate_b = NativeScalarFutureAwait::open(owner, 21);
    const auto duplicate_a_ticket = duplicate_a->id();
    const auto duplicate_b_ticket = duplicate_b->id();
    check(duplicate_a_ticket.runtime_id == duplicate_b_ticket.runtime_id
              && duplicate_a_ticket.actor_id == duplicate_b_ticket.actor_id
              && duplicate_a_ticket.generation == duplicate_b_ticket.generation
              && duplicate_a_ticket.future_id == duplicate_b_ticket.future_id,
          "duplicate Future numeric identity setup failed");
    check(!duplicate_b->complete(duplicate_a_ticket, 91),
          "duplicate numeric Future ticket settled another frame");
    check(!duplicate_b->cancel(duplicate_a_ticket),
          "duplicate numeric Future ticket cancelled another frame");
    check(duplicate_b->complete(duplicate_b_ticket, 92),
          "own frame ticket did not settle duplicate-id Future");
    int duplicate_resumed = 0;
    check(!duplicate_b->try_schedule(
              duplicate_a_ticket, group, owner,
              [&](const auto&) { ++duplicate_resumed; }),
          "duplicate numeric Future ticket scheduled another frame");
    check(duplicate_b->try_schedule(
              duplicate_b_ticket, group, owner,
              [&](const NativeScalarFutureAwait::Result& outcome) {
                  check(outcome.success && outcome.value == 92,
                        "duplicate-id own result corrupt");
                  ++duplicate_resumed;
              }),
          "own duplicate-id Future ticket did not schedule");
    check(group.run_control_turn(owner)
              == NativeActorGroup::ControlTurn::executed,
          "duplicate-id CONTROL did not run");
    check(duplicate_resumed == 1,
          "duplicate-id Future did not resume exactly once");
    check(duplicate_a->cancel(duplicate_a_ticket),
          "unused duplicate-id Future did not cancel with own ticket");

    // A failed value must be delivered (not treated as cancellation), and
    // a future ID from the same actor must not authorize an unrelated frame.
    auto failed = NativeScalarFutureAwait::open(owner, 19);
    auto other_ticket = failed->id();
    other_ticket.future_id += 1;
    check(!failed->fail(other_ticket, 17), "wrong future ID settled failure");
    check(failed->fail(failed->id(), 17), "failure not settled");
    int reported_failure = 0;
    check(failed->try_schedule(failed->id(), group, owner,
        [&](const NativeScalarFutureAwait::Result& outcome) {
            check(!outcome.success && outcome.value == 0 &&
                  outcome.error_code == 17, "failure result was mangled");
            ++reported_failure;
        }), "failure CONTROL rejected");
    check(reported_failure == 0, "failure callback executed inline");
    check(group.run_control_turn(owner) == NativeActorGroup::ControlTurn::executed,
          "failure CONTROL did not run");
    check(reported_failure == 1, "failure result not delivered once");
    check(!failed->complete(failed->id(), 42), "failed future resurrected");

    // Once the CONTROL callback has actually started, cancellation is too
    // late. The frame stays RUNNING until the callback exits, then COMPLETED.
    auto running = NativeScalarFutureAwait::open(owner, 20);
    check(running->complete(running->id(), 23), "running future not settled");
    std::atomic<int> callback_started{0};
    std::atomic<int> callback_release{0};
    std::atomic<int> callback_finished{0};
    check(running->try_schedule(running->id(), group, owner,
        [&](const NativeScalarFutureAwait::Result& outcome) {
            check(outcome.success && outcome.value == 23, "running result corrupt");
            callback_started.store(1, std::memory_order_release);
            while (!callback_release.load(std::memory_order_acquire))
                std::this_thread::yield();
            callback_finished.store(1, std::memory_order_release);
        }), "running CONTROL not queued");
    std::thread control_runner([&] {
        check(group.run_control_turn(owner)
              == NativeActorGroup::ControlTurn::executed,
              "running CONTROL turn failed");
    });
    while (!callback_started.load(std::memory_order_acquire))
        std::this_thread::yield();
    check(running->phase() == NativeScalarFutureAwait::Phase::running,
          "callback did not publish running phase");
    check(!running->cancel(running->id()),
          "started callback was incorrectly revoked");
    callback_release.store(1, std::memory_order_release);
    control_runner.join();
    check(callback_finished.load(std::memory_order_acquire) == 1,
          "running callback did not finish");
    check(running->phase() == NativeScalarFutureAwait::Phase::completed,
          "running callback did not complete");

    // Scheduling and cancellation may race while CONTROL admission occurs.
    // Neither path may hang a carrier or resurrect a cancelled Future.
    for (int iteration = 0; iteration < 512; ++iteration) {
        NativeActorGroup race_group(9750 + iteration, 1);
        auto race_owner = race_group.adopt(
            9750 + iteration, 1, 1, [] {});
        check(race_group.mark_ready(race_owner), "race owner not ready");
        auto race_finalizer = race_group.runtime_finalizer(race_owner);
        auto race = NativeScalarFutureAwait::open(race_owner, 2000 + iteration);
        check(race->complete(race->id(), iteration), "race settle failed");
        std::atomic<int> resumed{0};
        std::atomic<int> start{0};
        bool scheduled = false;
        bool cancelled = false;
        std::thread scheduler([&] {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            scheduled = race->try_schedule(
                race->id(), race_group, race_owner,
                [&](const NativeScalarFutureAwait::Result& outcome) {
                    check(outcome.success && outcome.value == iteration,
                          "schedule/cancel race result corrupt");
                    resumed.fetch_add(1, std::memory_order_relaxed);
                });
        });
        std::thread canceller([&] {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            cancelled = race->cancel(race->id());
        });
        start.store(1, std::memory_order_release);
        scheduler.join();
        canceller.join();

        if (race_group.has_pending_control(race_owner))
            (void)race_group.run_control_turn(race_owner);

        check(resumed.load(std::memory_order_relaxed) <= 1,
              "schedule/cancel race resumed twice");
        const auto phase = race->phase();
        if (cancelled) {
            check(phase == NativeScalarFutureAwait::Phase::cancelled,
                  "cancel winner did not remain terminal");
            check(resumed.load(std::memory_order_relaxed) == 0,
                  "cancelled race executed guest callback");
        } else if (scheduled) {
            check(phase == NativeScalarFutureAwait::Phase::completed,
                  "scheduled race did not complete");
            check(resumed.load(std::memory_order_relaxed) == 1,
                  "scheduled race did not resume exactly once");
        } else {
            check(phase == NativeScalarFutureAwait::Phase::ready,
                  "failed admission changed ready Future");
        }
        race_group.close();
        check(race->cancel_if_owner_detached(race->id(), race_owner)
                  || phase == NativeScalarFutureAwait::Phase::cancelled
                  || phase == NativeScalarFutureAwait::Phase::completed,
              "race teardown did not reach terminal state");
        check(race_finalizer.ready_for_resource_reclamation(),
              "race Future still pinned group finalization");
        check(race_finalizer.finalized(), "race actor did not finalize");
    }

    // Owner detachment is authoritative even when a producer has already
    // entered the transient SETTLING state. A one-shot teardown cancellation
    // must never leave a detached Future READY.
    for (int iteration = 0; iteration < 256; ++iteration) {
        NativeActorGroup settle_group(12000 + iteration, 1);
        auto settle_owner = settle_group.adopt(
            12000 + iteration, 1, 1, [] {});
        check(settle_group.mark_ready(settle_owner), "settle owner not ready");
        auto settle_finalizer = settle_group.runtime_finalizer(settle_owner);
        auto settle_future = NativeScalarFutureAwait::open(
            settle_owner, 4000 + iteration);
        std::atomic<int> start_settle{0};
        bool producer_won = false;
        bool teardown_accepted = false;
        std::thread producer([&] {
            while (!start_settle.load(std::memory_order_acquire))
                std::this_thread::yield();
            producer_won = settle_future->complete(
                settle_future->id(), iteration);
        });
        std::thread teardown([&] {
            while (!start_settle.load(std::memory_order_acquire))
                std::this_thread::yield();
            settle_group.close();
            teardown_accepted = settle_future->cancel_if_owner_detached(
                settle_future->id(), settle_owner);
        });
        start_settle.store(1, std::memory_order_release);
        producer.join();
        teardown.join();

        check(teardown_accepted, "detached settling Future missed cancellation");
        check(settle_future->phase()
                  == NativeScalarFutureAwait::Phase::cancelled,
              "detached settling Future became non-cancelled");
        if (!producer_won)
            check(!settle_future->complete(settle_future->id(), iteration),
                  "cancel winner allowed producer resurrection");
        check(settle_finalizer.ready_for_resource_reclamation(),
              "settle/detach race pinned finalization");
        check(settle_finalizer.finalized(),
              "settle/detach race actor did not finalize");
    }

    // Three terminal contenders race. Exactly one wins and no duplicate
    // resumption is accepted, regardless of scheduling order.
    for (std::uint64_t n = 1; n <= 256; ++n) {
        auto raced = NativeScalarFutureAwait::open(owner, 100 + n);
        const auto ticket = raced->id();
        std::atomic<int> wins{0};
        std::thread a([&] { if (raced->complete(ticket, 5)) ++wins; });
        std::thread b([&] { if (raced->fail(ticket, 9)) ++wins; });
        std::thread c([&] { if (raced->cancel(ticket)) ++wins; });
        a.join(); b.join(); c.join();
        check(wins == 1, "terminal transition did not have one winner");
        if (raced->phase() == NativeScalarFutureAwait::Phase::ready) {
            int callbacks = 0;
            check(raced->try_schedule(ticket, group, owner,
                [&](const auto&) { ++callbacks; }), "settled race not scheduled");
            check(group.run_control_turn(owner)
                  == NativeActorGroup::ControlTurn::executed, "CONTROL race lost");
            check(callbacks == 1, "raced future callback count");
        } else {
            check(raced->phase() == NativeScalarFutureAwait::Phase::cancelled,
                  "race ended in invalid state");
        }
    }
    check(group.finalized(owner) && group.finalized(foreign), "group not quiescent");

    // Per-actor physical finalization also removes an incarnation from the
    // delivery table while the group remains open. Publish the same closed
    // barrier used by group close so orphaned scalar Futures become terminal.
    NativeActorGroup finalized_group(9790, 2);
    auto finalized_owner = finalized_group.adopt(9790, 1, 1, [] {});
    auto live_sibling = finalized_group.adopt(9790, 2, 1, [] {});
    check(finalized_group.mark_ready(finalized_owner)
          && finalized_group.mark_ready(live_sibling),
          "finalized-group admission failed");
    auto finalized_ready = NativeScalarFutureAwait::open(finalized_owner, 70);
    auto finalized_pending = NativeScalarFutureAwait::open(finalized_owner, 71);
    check(finalized_ready->complete(finalized_ready->id(), 55),
          "per-actor finalized READY settle failed");
    check(!finalized_owner.delivery_closed(),
          "delivery closed before per-actor finalization");
    check(finalized_group.finalized(finalized_owner),
          "per-actor finalization failed");
    check(finalized_owner.delivery_closed(),
          "per-actor finalization did not publish delivery barrier");
    check(!finalized_ready->try_schedule(
              finalized_ready->id(), finalized_group, finalized_owner,
              [&](const auto&) { check(false, "finalized READY callback executed"); }),
          "finalized READY Future reported scheduled");
    check(!finalized_pending->try_schedule(
              finalized_pending->id(), finalized_group, finalized_owner,
              [&](const auto&) { check(false, "finalized PENDING callback executed"); }),
          "finalized PENDING Future reported scheduled");
    check(finalized_ready->phase() == NativeScalarFutureAwait::Phase::cancelled,
          "finalized READY Future did not cancel");
    check(finalized_pending->phase() == NativeScalarFutureAwait::Phase::cancelled,
          "finalized PENDING Future did not cancel");
    int sibling_turns = 0;
    check(finalized_group.run_user_turn(live_sibling, [&] { ++sibling_turns; })
              == NativeActorGroup::UserTurn::executed,
          "sibling actor stopped when peer finalized");
    check(sibling_turns == 1, "live sibling did not execute");
    check(finalized_group.finalized(live_sibling),
          "live sibling did not finalize");

    // Group close may drop an already-reserved CONTROL callback. The Future
    // must observe that generation-fenced loss and become cancellable rather
    // than remaining permanently scheduled.
    NativeActorGroup teardown_group(9801, 1);
    auto teardown_owner = teardown_group.adopt(9801, 1, 1, [] {});
    check(teardown_group.mark_ready(teardown_owner), "teardown owner not ready");
    auto finalizer = teardown_group.runtime_finalizer(teardown_owner);
    auto dropped = NativeScalarFutureAwait::open(teardown_owner, 77);
    check(dropped->complete(dropped->id(), 99), "teardown future not settled");
    int dropped_callbacks = 0;
    check(dropped->try_schedule(dropped->id(), teardown_group, teardown_owner,
        [&](const auto&) { ++dropped_callbacks; }), "teardown CONTROL not queued");
    check(!teardown_owner.control_delivery_lost(), "CONTROL lost before close");
    teardown_group.close();
    check(teardown_owner.control_delivery_lost(), "dropped CONTROL not published");
    check(dropped->cancel_if_owner_detached(dropped->id(), teardown_owner),
          "dropped CONTROL did not revoke Future");
    check(dropped->phase() == NativeScalarFutureAwait::Phase::cancelled,
          "revoked Future did not become cancelled");
    check(dropped_callbacks == 0, "dropped callback executed");
    check(finalizer.ready_for_resource_reclamation(),
          "dropped CONTROL still pins finalization");
    check(finalizer.finalized(), "detached actor not finalized");

    // Detachment must also revoke frames that were READY or still PENDING
    // without relying on an unrelated sticky "CONTROL dropped" bit.
    NativeActorGroup detached_group(9802, 1);
    auto detached_owner = detached_group.adopt(9802, 1, 1, [] {});
    check(detached_group.mark_ready(detached_owner), "detached owner not ready");
    auto detached_finalizer = detached_group.runtime_finalizer(detached_owner);
    auto ready_only = NativeScalarFutureAwait::open(detached_owner, 78);
    auto pending_only = NativeScalarFutureAwait::open(detached_owner, 79);
    check(ready_only->complete(ready_only->id(), 7), "ready-only settle failed");
    check(!detached_owner.control_delivery_lost(),
          "unexpected CONTROL loss before detach");
    detached_group.close();
    check(detached_owner.delivery_closed(), "detach barrier not published");
    check(!detached_owner.control_delivery_lost(),
          "detach without queued CONTROL reported false drop");
    check(ready_only->cancel_if_owner_detached(
              ready_only->id(), detached_owner),
          "detached READY Future not cancelled");
    check(pending_only->cancel_if_owner_detached(
              pending_only->id(), detached_owner),
          "detached PENDING Future not cancelled");
    check(ready_only->phase() == NativeScalarFutureAwait::Phase::cancelled,
          "detached READY phase");
    check(pending_only->phase() == NativeScalarFutureAwait::Phase::cancelled,
          "detached PENDING phase");
    check(detached_finalizer.ready_for_resource_reclamation(),
          "detached owner still pinned");
    check(detached_finalizer.finalized(), "detached owner finalization failed");

    // Scheduling itself must consume an already-published detach barrier.
    // Callers must not need a second teardown reconciliation just because
    // they discovered close through try_schedule().
    NativeActorGroup closed_schedule_group(9803, 1);
    auto closed_schedule_owner = closed_schedule_group.adopt(9803, 1, 1, [] {});
    check(closed_schedule_group.mark_ready(closed_schedule_owner),
          "closed-schedule owner not ready");
    auto closed_schedule_finalizer =
        closed_schedule_group.runtime_finalizer(closed_schedule_owner);
    auto closed_ready = NativeScalarFutureAwait::open(closed_schedule_owner, 82);
    auto closed_pending = NativeScalarFutureAwait::open(closed_schedule_owner, 83);
    check(closed_ready->complete(closed_ready->id(), 13),
          "closed-schedule READY settle failed");
    closed_schedule_group.close();
    check(closed_schedule_owner.delivery_closed(),
          "closed-schedule barrier not published");
    check(!closed_ready->try_schedule(
              closed_ready->id(), closed_schedule_group, closed_schedule_owner,
              [&](const auto&) { check(false, "detached READY callback executed"); }),
          "detached READY Future reported scheduled");
    check(!closed_pending->try_schedule(
              closed_pending->id(), closed_schedule_group, closed_schedule_owner,
              [&](const auto&) { check(false, "detached PENDING callback executed"); }),
          "detached PENDING Future reported scheduled");
    check(closed_ready->phase() == NativeScalarFutureAwait::Phase::cancelled,
          "detached READY schedule did not cancel");
    check(closed_pending->phase() == NativeScalarFutureAwait::Phase::cancelled,
          "detached PENDING schedule did not cancel");
    check(closed_schedule_finalizer.ready_for_resource_reclamation(),
          "closed-schedule owner still pinned");
    check(closed_schedule_finalizer.finalized(),
          "closed-schedule owner finalization failed");

    // A detached old generation must never revoke a Future owned by a newer
    // generation reusing the same runtime/actor identity.
    NativeActorGroup next_group(9802, 1);
    auto next_owner = next_group.adopt(9802, 1, 2, [] {});
    check(next_group.mark_ready(next_owner), "next generation not ready");
    auto next_finalizer = next_group.runtime_finalizer(next_owner);
    auto next_future = NativeScalarFutureAwait::open(next_owner, 80);
    check(next_future->complete(next_future->id(), 11),
          "next generation Future not settled");
    check(!next_future->cancel_if_owner_detached(
              next_future->id(), detached_owner),
          "stale detached ref revoked newer generation");
    check(next_future->phase() == NativeScalarFutureAwait::Phase::ready,
          "newer generation phase changed through stale ref");
    next_group.close();
    check(next_future->cancel_if_owner_detached(
              next_future->id(), next_owner),
          "current detached owner did not revoke Future");
    check(next_finalizer.ready_for_resource_reclamation(),
          "next generation still pinned after revoke");
    check(next_finalizer.finalized(), "next generation finalization failed");

    // Even if a buggy host reuses the same numeric generation after physical
    // finalization, the old ActorRef ticket is not the new actor capability.
    NativeActorGroup reused_group(9802, 1);
    auto reused_owner = reused_group.adopt(9802, 1, 1, [] {});
    check(reused_group.mark_ready(reused_owner), "reused generation not ready");
    auto reused_finalizer = reused_group.runtime_finalizer(reused_owner);
    auto reused_future = NativeScalarFutureAwait::open(reused_owner, 81);
    check(reused_future->complete(reused_future->id(), 12),
          "reused generation Future not settled");
    check(!detached_owner.same_incarnation(reused_owner),
          "distinct actor lifetimes shared capability ticket");
    check(!reused_future->try_schedule(
              reused_future->id(), reused_group, detached_owner,
              [&](const auto&) {}),
          "old same-generation capability scheduled newer Future");
    check(!reused_future->cancel_if_owner_detached(
              reused_future->id(), detached_owner),
          "old same-generation capability revoked newer Future");
    reused_group.close();
    check(reused_future->cancel_if_owner_detached(
              reused_future->id(), reused_owner),
          "current reused capability did not revoke Future");
    check(reused_finalizer.ready_for_resource_reclamation(),
          "reused generation still pinned");
    check(reused_finalizer.finalized(), "reused generation finalization failed");

    return 0;
}
