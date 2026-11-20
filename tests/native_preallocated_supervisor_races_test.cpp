// Native host integration: real preallocated BoundedActorPort,
// PriorityActorInbox, and NativeActorGroup supervisor turn gate.
// NO guest .atto AOT or hard-RT qualification is claimed.
#include "oreslang/runtime/PriorityActorInbox.hpp"
#include <atomic>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>
using oreslang::runtime::NativeActorGroup;
using oreslang::runtime::PriorityActorInbox;
#define CHECK(x) do { if (!(x)) throw std::runtime_error("supervisor mailbox fence: " #x); } while(false)
using Inbox = PriorityActorInbox<int>;
using Lane = Inbox::Lane;

static void control_priority_resumable_selective_and_abort() {
    NativeActorGroup group(43001, 1);
    auto actor = group.adopt(43001, 11, 1, [] {});
    auto finalizer = group.runtime_finalizer(actor);
    CHECK(group.mark_ready(actor));
    Inbox inbox({2, 32}, {4, 64}, {2, 32}, 8);
    inbox.bind_target(actor);
    CHECK(inbox.offer_to(actor, Lane::Normal, 11, 8, 11));
    CHECK(inbox.offer_to(actor, Lane::Normal, 17, 8, 17));
    CHECK(inbox.offer_to(actor, Lane::Normal, 23, 8, 23));
    CHECK(inbox.snapshot() == 3 && actor.outstanding_resource_loans() == 3);
    bool denied = false;
    try { (void)inbox.try_receive(); } catch (const std::logic_error&) { denied = true; }
    CHECK(denied);
    denied = false;
    try { (void)inbox.try_receive_tagged(Lane::Normal, 23, 1); }
    catch (const std::logic_error&) { denied = true; }
    CHECK(denied);
    bool controlled = false, consumed = false;
    CHECK(group.enqueue_control(actor, [&] { controlled = true; }));
    Inbox::SelectiveCursor cursor;
    auto process = [&](Inbox::Delivery& receipt) {
        CHECK(!consumed && receipt.value() == 23 && receipt.sequence() == 3);
        consumed = true;
        CHECK(receipt.acknowledge_released());
        CHECK(!receipt.acknowledge_released());
    };
    using S = Inbox::SelectiveDispatch;
    CHECK(inbox.dispatch_next_tagged(group, actor, Lane::Normal, 23, 1,
                                     cursor, process) == S::Control);
    CHECK(controlled && !consumed);
    CHECK(inbox.dispatch_next_tagged(group, actor, Lane::Normal, 23, 1,
                                     cursor, process) == S::Resume);
    CHECK(inbox.dispatch_next_tagged(group, actor, Lane::Normal, 23, 1,
                                     cursor, process) == S::Resume);
    CHECK(inbox.dispatch_next_tagged(group, actor, Lane::Normal, 23, 1,
                                     cursor, process) == S::User);
    CHECK(consumed && inbox.used_frames(Lane::Normal) == 2);
    CHECK(inbox.watermark() == 0); // sequence 3 cannot overtake 1 and 2
    group.close();
    CHECK(inbox.dispatch_next_tagged(group, actor, Lane::Normal, 23, 1,
                                     cursor, process) == S::Retired);
    CHECK(finalizer.ready_for_actor_turn_quiescence());
    CHECK(inbox.retire_queued_after_quiescence(finalizer, actor) == 2);
    CHECK(inbox.watermark() == 3);
    CHECK(inbox.drain_status(3) == Inbox::DrainStatus::Aborted);
    CHECK(!inbox.completed_through(3));
    CHECK(actor.outstanding_resource_loans() == 0);
    CHECK(finalizer.ready_for_resource_reclamation());
    CHECK(finalizer.finalized());
    actor.get_done().get();
    inbox.close();
}

static void active_user_turn_stop_and_control_race() {
    NativeActorGroup group(43002, 1);
    auto actor = group.adopt(43002, 11, 1, [] {});
    auto finalizer = group.runtime_finalizer(actor);
    CHECK(group.mark_ready(actor));
    Inbox inbox({2, 32}, {2, 32}, {2, 32}, 4);
    inbox.bind_target(actor);
    CHECK(inbox.offer_to(actor, Lane::Normal, 101, 8));
    CHECK(inbox.offer_to(actor, Lane::Urgent, 202, 8));
    std::atomic<bool> in_turn{false}, release{false}, done{false};
    std::thread carrier([&] {
        CHECK(inbox.dispatch_next(group, actor, [&](Inbox::Delivery& receipt) {
            CHECK(receipt.lane() == Lane::Urgent && receipt.value() == 202);
            in_turn.store(true, std::memory_order_release);
            while (!release.load(std::memory_order_acquire)) std::this_thread::yield();
            CHECK(receipt.acknowledge_released());
        }) == Inbox::Dispatch::User);
        done.store(true, std::memory_order_release);
    });
    while (!in_turn.load(std::memory_order_acquire)) std::this_thread::yield();
    CHECK(group.enqueue_control(actor, [] {}));
    CHECK(inbox.dispatch_next(group, actor, [&](Inbox::Delivery&) {
        CHECK(false && "concurrent actor user turns are forbidden");
    }) == Inbox::Dispatch::Busy);
    CHECK(!finalizer.ready_for_actor_turn_quiescence());
    group.close();
    CHECK(actor.control_delivery_lost());
    CHECK(!finalizer.ready_for_actor_turn_quiescence());
    CHECK(inbox.dispatch_next(group, actor, [&](Inbox::Delivery&) {
        CHECK(false && "retired actor cannot consume user mail");
    }) == Inbox::Dispatch::Retired);
    bool rejected = false;
    try { (void)inbox.retire_queued_after_quiescence(finalizer, actor); }
    catch (const std::logic_error&) { rejected = true; }
    CHECK(rejected);
    release.store(true, std::memory_order_release);
    carrier.join();
    CHECK(done.load(std::memory_order_acquire));
    CHECK(finalizer.ready_for_actor_turn_quiescence());
    CHECK(inbox.retire_queued_after_quiescence(finalizer, actor) == 1);
    CHECK(inbox.snapshot() == 2 && inbox.watermark() == 2);
    CHECK(inbox.drain_status(2) == Inbox::DrainStatus::Aborted);
    CHECK(actor.outstanding_resource_loans() == 0);
    CHECK(finalizer.ready_for_resource_reclamation());
    CHECK(finalizer.finalized());
    actor.get_done().get();
    inbox.close();
}

static void raced_send_stop_must_abort_only_winning_publications() {
    unsigned send_wins = 0, stop_wins = 0;
    for (unsigned trial = 0; trial < 400; ++trial) {
        const std::uint64_t runtime = 50000 + trial;
        NativeActorGroup group(runtime, 1);
        auto actor = group.adopt(runtime, 11, 1, [] {});
        CHECK(group.mark_ready(actor));
        auto finalizer = group.runtime_finalizer(actor);
        Inbox inbox({1, 16}, {1, 16}, {1, 16}, 2);
        inbox.bind_target(actor);
        auto owned = std::make_unique<int>(static_cast<int>(trial));
        std::atomic<unsigned> barrier{0};
        bool accepted = false, rejected = false;
        std::thread sender([&] {
            barrier.fetch_add(1, std::memory_order_acq_rel);
            while (barrier.load(std::memory_order_acquire) != 2)
                std::this_thread::yield();
            try { accepted = inbox.offer_owned_to(actor, Lane::Normal, owned, 8); }
            catch (const std::logic_error&) { rejected = true; }
        });
        std::thread stopper([&] {
            barrier.fetch_add(1, std::memory_order_acq_rel);
            while (barrier.load(std::memory_order_acquire) != 2)
                std::this_thread::yield();
            group.stop_workers();
        });
        sender.join();
        stopper.join();
        CHECK(accepted != rejected);
        CHECK(accepted ? !owned : bool(owned));
        CHECK(inbox.snapshot() == (accepted ? 1u : 0u));
        group.close();
        CHECK(finalizer.ready_for_actor_turn_quiescence());
        CHECK(inbox.retire_queued_after_quiescence(finalizer, actor) ==
              (accepted ? 1u : 0u));
        CHECK(inbox.retire_queued_after_quiescence(finalizer, actor) == 0);
        if (accepted) {
            ++send_wins;
            CHECK(inbox.drain_status(1) == Inbox::DrainStatus::Aborted);
            CHECK(!inbox.completed_through(1));
        } else ++stop_wins;
        CHECK(actor.outstanding_resource_loans() == 0);
        CHECK(finalizer.ready_for_resource_reclamation());
        CHECK(finalizer.finalized());
        actor.get_done().get();
        inbox.close();
    }
    CHECK(send_wins + stop_wins == 400);
}

// Cross-backend admission/fence contract: the real native mailbox gate
// serializes DATA issuance with close, and no post-close producer can acquire
// new authority. Pre-close accepted envelopes must receive an aborted drain
// receipt before the native supervisor may reclaim the actor generation.
static void supervisor_parity_close_races_data_and_retires_loans() {
    for (unsigned trial = 0; trial < 128; ++trial) {
        const std::uint64_t runtime = 910000 + trial;
        NativeActorGroup group(runtime, 1);
        auto actor = group.adopt(runtime, 17, 1, [] {});
        auto finalizer = group.runtime_finalizer(actor);
        CHECK(group.mark_ready(actor));
        Inbox inbox({1, 16}, {2, 32}, {1, 16}, 3);
        inbox.bind_target(actor);
        // Require an already-accepted DATA loan before the close race;
        // a run in which STOP always wins cannot masquerade as drain proof.
        CHECK(inbox.offer_to(actor, Lane::Normal, 100000 + static_cast<int>(trial), 8));
        CHECK(actor.outstanding_resource_loans() == 1);
        std::atomic<unsigned> started{0};
        bool admitted = false, denied = false;
        std::exception_ptr closer_error;
        std::thread producer([&] {
            started.fetch_add(1, std::memory_order_release);
            while (started.load(std::memory_order_acquire) != 2)
                std::this_thread::yield();
            try {
                admitted = inbox.offer_to(actor, Lane::Normal,
                                         static_cast<int>(trial), 8);
                denied = !admitted;
            } catch (const std::logic_error&) {
                denied = true;
            }
        });
        std::thread closer([&] {
            started.fetch_add(1, std::memory_order_release);
            while (started.load(std::memory_order_acquire) != 2)
                std::this_thread::yield();
            // close() may report still outstanding pre-fence loans; the
            // actual seal must remain sticky even in that case.
            try { inbox.close(); }
            catch (const std::logic_error&) {}
            catch (...) { closer_error = std::current_exception(); }
        });
        producer.join();
        closer.join();
        if (closer_error) std::rethrow_exception(closer_error);
        CHECK(admitted != denied);
        CHECK(inbox.snapshot() == (admitted ? 2u : 1u));
        bool post_fence_denied = false;
        try { (void)inbox.offer_to(actor, Lane::Normal, -1, 8); }
        catch (const std::logic_error&) { post_fence_denied = true; }
        CHECK(post_fence_denied);
        group.close();
        CHECK(finalizer.ready_for_actor_turn_quiescence());
        CHECK(inbox.retire_queued_after_quiescence(finalizer, actor)
              == (admitted ? 2u : 1u));
        CHECK(inbox.drain_status(1) == Inbox::DrainStatus::Aborted);
        if (admitted) CHECK(inbox.drain_status(2) == Inbox::DrainStatus::Aborted);
        CHECK(actor.outstanding_resource_loans() == 0);
        CHECK(finalizer.ready_for_resource_reclamation());
        CHECK(finalizer.finalized());
        actor.get_done().get();
    }
}

int main() {
    control_priority_resumable_selective_and_abort();
    active_user_turn_stop_and_control_race();
    raced_send_stop_must_abort_only_winning_publications();
    supervisor_parity_close_races_data_and_retires_loans();
    std::cout << "LLVM_SUPERVISOR_DATA_FENCE_HOST_PASS|new_races=128|guest_atto=0\n";
}
