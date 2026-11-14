#include "oreslang/runtime/ActorGroupLifecycle.hpp"
#include "oreslang/runtime/ActorOwnedI64Heap.h"
#include "oreslang/runtime/NativeActorRetirementBarrier.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <stdexcept>
#include <thread>

namespace oreslang::runtime {
struct NativeSourceCleanupGateTestPeer {
    static bool claim(NativeSourceCleanupGate& g, NativeSourceCleanupTicket t) noexcept {
        return g.try_claim_cleanup(t);
    }
    static bool yield(NativeSourceCleanupGate& g, NativeSourceCleanupTicket t) noexcept {
        return g.yield_cleanup(t);
    }
    static bool complete(NativeSourceCleanupGate& g, NativeSourceCleanupTicket t) noexcept {
        return g.complete_cleanup(t);
    }
};
} // namespace oreslang::runtime

using namespace oreslang::runtime;
using Poll = NativeActorRetirementBarrier::Poll;
using Kind = ActorResourceRegistry::Kind;

static void require(bool value, const char* why) {
    if (!value) throw std::runtime_error(why);
}

static void successful_heap_retirement() {
    NativeActorResourceQuarantine quarantine(2);
    NativeActorGroup group(301, 1);
    auto registry = std::make_shared<ActorResourceRegistry>(7, 3);

    OresActorI64Heap* heap = ores_actor_i64_heap_open(7, 3, 4);
    require(heap != nullptr, "heap admission");
    const auto turn = ores_actor_i64_heap_enter(heap, 7, 3);
    const auto slot = ores_actor_i64_heap_new(heap, turn, 123);
    const auto loan = ores_actor_i64_heap_borrow(heap, turn, slot);
    ores_actor_i64_heap_leave(heap, turn);

    // Runtime-owned native arena allocation. The destroy operation may run
    // only after both the registry and actual heap have quiesced.
    registry->register_resource(Kind::native_allocation, [heap] {
        if (!ores_actor_i64_heap_try_destroy(heap))
            throw std::runtime_error("native heap still pinned");
    });
    const auto registryLoan = registry->acquire_loan();
    NativeActorRetirementBarrier* barrier = nullptr;
    auto ref = group.adopt(301, 7, 3, [&] {
        ores_actor_i64_heap_retire(heap);
        barrier->retire();
    });
    auto finalizer = group.runtime_finalizer(ref);
    NativeActorRetirementBarrier owned(registry, finalizer, quarantine);
    barrier = &owned;
    require(group.mark_ready(ref), "actor readiness");
    require(owned.poll() == Poll::not_retired, "done before stop");
    group.close();
    require(owned.poll() == Poll::pending, "loan must pin actor done");
    require(ref.get_done().wait_for(std::chrono::milliseconds(0))
                    == std::future_status::timeout, "done settled with loan");
    ores_actor_i64_heap_release(heap, loan);
    registry->release_loan(registryLoan);
    require(owned.poll() == Poll::completed, "heap did not reclaim");
    require(quarantine.size() == 0, "success entered quarantine");
    ref.get_done().get();
    require(owned.poll() == Poll::completed, "duplicate done");
}

static void failed_ffi_revocation_is_quarantined() {
    NativeActorResourceQuarantine quarantine(1);
    std::weak_ptr<int> retained;
    {
        auto sentinel = std::make_shared<int>(101);
        retained = sentinel;
        NativeActorGroup group(302, 2);
        auto registry = std::make_shared<ActorResourceRegistry>(8, 1);
        registry->register_resource(Kind::native_allocation, [sentinel] { });
        registry->register_resource(Kind::ffi_callback, [] {
            throw std::runtime_error("could not revoke callback");
        });
        NativeActorRetirementBarrier* barrier = nullptr;
        auto ref = group.adopt(302, 8, 1, [&] { barrier->retire(); });
        NativeActorRetirementBarrier owned(registry, group.runtime_finalizer(ref),
                                           quarantine);
        barrier = &owned;
        require(group.mark_ready(ref), "actor readiness");
        group.close();
        require(owned.poll() == Poll::failed, "unsafe close must fail done");
        bool exceptional = false;
        try { ref.get_done().get(); }
        catch (const std::exception&) { exceptional = true; }
        require(exceptional, "get_done unexpectedly succeeded after failed revocation");
        require(quarantine.size() == 1, "failed registry not retained");
        require(owned.poll() == Poll::failed, "failed close retried");
        sentinel.reset();
    }
    require(!retained.expired(),
            "quarantine lost captured ownership after actor/group destruction");
    // This test has no real outstanding external FFI references. Its mock
    // producer has been fully stopped; explicitly discharge quarantine.
    quarantine.release_after_verified_isolate_shutdown();
    require(retained.expired(), "verified quarantine discharge retained callback");
}

static void quarantine_saturation_does_not_settle_done() {
    NativeActorResourceQuarantine quarantine(1);
    NativeActorGroup group(303, 2);
    auto first = std::make_shared<ActorResourceRegistry>(9, 1);
    auto second = std::make_shared<ActorResourceRegistry>(10, 1);
    first->register_resource(Kind::ffi_callback, [] { throw std::runtime_error("first"); });
    second->register_resource(Kind::ffi_callback, [] { throw std::runtime_error("second"); });
    NativeActorRetirementBarrier* b1 = nullptr;
    NativeActorRetirementBarrier* b2 = nullptr;
    auto r1 = group.adopt(303, 9, 1, [&] { b1->retire(); });
    auto r2 = group.adopt(303, 10, 1, [&] { b2->retire(); });
    NativeActorRetirementBarrier barrier1(first, group.runtime_finalizer(r1),
                                           quarantine);
    NativeActorRetirementBarrier barrier2(second, group.runtime_finalizer(r2),
                                           quarantine);
    b1 = &barrier1;
    b2 = &barrier2;
    require(group.mark_ready(r1) && group.mark_ready(r2), "actor ready");
    group.close();
    require(barrier1.poll() == Poll::failed, "first failure");
    require(barrier2.poll() == Poll::quarantine_full, "quarantine capacity");
    require(r2.get_done().wait_for(std::chrono::milliseconds(0))
                    == std::future_status::timeout,
            "quarantine full fabricated physical done");
    // A bounded quarantine must not release captures simply because its
    // owner cannot admit another failed registry. Only verified isolation is
    // permission to discharge BOTH registered and overflowed failures.
    require(second->quarantined_resources() == 1,
            "overflowed failure lost its callback capture");
    bool prematureDischargeRejected = false;
    try {
        quarantine.release_overflow_after_verified_isolate_shutdown(second);
    } catch (const std::logic_error&) {
        prematureDischargeRejected = true;
    }
    require(prematureDischargeRejected, "overflow discharge bypassed isolate fence");
    // Mock FFI producers have now been stopped; the test supervisor verifies
    // their isolation and explicitly releases captured ownership.
    quarantine.release_after_verified_isolate_shutdown();
    require(quarantine.isolate_shutdown_verified(), "shutdown verification lost");
    require(barrier2.poll() == Poll::failed,
            "overflowed actor must now fail done after verified isolation");
    bool doneFailed = false;
    try { r2.get_done().get(); }
    catch (const std::exception&) { doneFailed = true; }
    require(doneFailed, "overflowed actor fabricated successful done");
    require(second->quarantined_resources() == 0,
            "verified overflow discharge retained native callbacks");
    require(barrier2.poll() == Poll::failed,
            "overflowed actor re-entered native closer after terminal failure");
    require(!quarantine.hold(first),
            "verified isolation quarantine must remain sealed");
}

static void physical_reclamation_waits_for_admitted_actor_turn() {
    NativeActorResourceQuarantine quarantine(1);
    NativeActorGroup group(305, 1);
    auto registry = std::make_shared<ActorResourceRegistry>(12, 1);
    std::atomic<int> freed{0};
    registry->register_resource(Kind::native_allocation, [&] { ++freed; });
    NativeActorRetirementBarrier* barrier = nullptr;
    auto ref = group.adopt(305, 12, 1, [&] { barrier->retire(); });
    NativeActorRetirementBarrier owned(registry, group.runtime_finalizer(ref), quarantine);
    barrier = &owned;
    require(group.mark_ready(ref), "ready");
    std::promise<void> entered, leave;
    auto arrived = entered.get_future();
    auto released = leave.get_future();
    std::thread guest([&] {
        auto admitted = group.run_user_turn(ref, [&] {
            entered.set_value();
            released.wait();  // deliberate host test barrier, NOT guest runtime behavior
        });
        require(admitted == NativeActorGroup::UserTurn::executed, "actor turn");
    });
    arrived.wait();
    group.close(); // logical stop may occur while admitted guest turn is active
    require(owned.poll() == Poll::not_admitted,
            "physical drop admitted before active actor turn released its lease");
    require(freed == 0, "native arena freed while guest still executing");
    require(ref.get_done().wait_for(std::chrono::milliseconds(0))
                    == std::future_status::timeout, "done was fabricated");
    leave.set_value();
    guest.join();
    require(owned.poll() == Poll::completed, "done did not follow guest exit");
    require(freed == 1, "resource drop did not occur exactly once");
    ref.get_done().get();
}

static void done_waits_for_group_detachment() {
    NativeActorResourceQuarantine quarantine(1);
    NativeActorGroup group(304, 1);
    auto registry = std::make_shared<ActorResourceRegistry>(11, 1);
    NativeActorRetirementBarrier* barrier = nullptr;
    auto ref = group.adopt(304, 11, 1, [&] { barrier->retire(); });
    NativeActorRetirementBarrier owned(registry, group.runtime_finalizer(ref), quarantine);
    barrier = &owned;
    require(group.mark_ready(ref), "actor ready");
    owned.retire();
    require(owned.poll() == Poll::not_admitted,
            "group-attached actor must not acknowledge done");
    require(ref.get_done().wait_for(std::chrono::milliseconds(0))
                    == std::future_status::timeout,
            "done settled before carrier/manager detached");
    group.close();
    require(owned.poll() == Poll::completed, "finalizer rejected after detach");
    ref.get_done().get();
}

static void foreign_registry_cannot_release_live_heap_callback() {
    NativeActorResourceQuarantine quarantine(1);
    NativeActorGroup group(306, 1);
    auto registry = std::make_shared<ActorResourceRegistry>(7, 3);
    ActorResourceRegistry foreign(7, 3);
    auto* heap = ores_actor_i64_heap_open(7, 3, 1);
    int freed = 0;
    registry->register_resource(Kind::native_allocation, [heap, &freed] {
        require(ores_actor_i64_heap_try_destroy(heap) == 1, "heap still pinned");
        ++freed;
    });
    // A real external callback must hold its registry pin until it stops
    // touching heap storage. Equal numeric identities do not release that pin.
    const auto held = registry->acquire_callback();
    const auto other = foreign.acquire_callback();
    NativeActorRetirementBarrier* barrier = nullptr;
    auto ref = group.adopt(306, 7, 3, [&] {
        ores_actor_i64_heap_retire(heap);
        barrier->retire();
    });
    NativeActorRetirementBarrier owned(registry, group.runtime_finalizer(ref), quarantine);
    barrier = &owned;
    require(group.mark_ready(ref), "actor readiness");
    group.close();
    bool rejected = false;
    try { registry->release_callback(other); }
    catch (const std::logic_error&) { rejected = true; }
    require(rejected, "foreign registry released the live heap callback");
    require(owned.poll() == Poll::pending && freed == 0,
            "foreign callback ticket authorized heap destruction");
    require(ref.get_done().wait_for(std::chrono::milliseconds(0))
                    == std::future_status::timeout, "foreign ticket fabricated done");
    registry->release_callback(held);
    require(owned.poll() == Poll::completed && freed == 1, "own callback did not drain heap");
    ref.get_done().get();
    require(owned.poll() == Poll::completed && freed == 1, "heap destroyed twice");
    foreign.release_callback(other);
    foreign.retire();
    require(foreign.try_reclaim().status == ActorResourceRegistry::Reclaim::complete,
            "foreign registry lost its own callback");
}

static void foreign_release_races_with_retirement_and_last_callback() {
    for (unsigned round = 0; round < 32; ++round) {
        NativeActorResourceQuarantine quarantine(1);
        NativeActorGroup group(307, 1);
        auto registry = std::make_shared<ActorResourceRegistry>(7, 3);
        ActorResourceRegistry foreign(7, 3);
        auto* heap = ores_actor_i64_heap_open(7, 3, 1);
        std::atomic<int> freed{0};
        registry->register_resource(Kind::native_allocation, [&] {
            require(ores_actor_i64_heap_try_destroy(heap) == 1, "heap still pinned");
            freed.fetch_add(1, std::memory_order_relaxed);
        });
        const auto held = registry->acquire_callback();
        const auto other = foreign.acquire_callback();
        NativeActorRetirementBarrier* barrier = nullptr;
        auto ref = group.adopt(307, 7, 3, [&] {
            ores_actor_i64_heap_retire(heap);
            barrier->retire();
        });
        NativeActorRetirementBarrier owned(registry, group.runtime_finalizer(ref), quarantine);
        barrier = &owned;
        require(group.mark_ready(ref), "actor readiness");
        std::atomic<bool> early_done{false}, final_release{false};
        std::exception_ptr attacker_error, release_error, stop_error;
        std::uint64_t rejected = 0;
        std::thread attacker([&] {
            const auto attempt = [&] {
                try { registry->release_callback(other); }
                catch (const std::logic_error&) { ++rejected; return; }
                throw std::runtime_error("foreign callback released live activity during race");
            };
            try { for (unsigned i = 0; i < 64; ++i) attempt(); }
            catch (...) { attacker_error = std::current_exception(); }
            early_done.store(true, std::memory_order_release);
            try {
                // Keep attacking while group.close retires the actor, then
                // race the valid final release and concurrent barrier polls.
                while (!final_release.load(std::memory_order_acquire)) {
                    attempt();
                    std::this_thread::yield();
                }
                for (unsigned i = 0; i < 128; ++i) attempt();
            }
            catch (...) { attacker_error = std::current_exception(); }
        });
        std::thread stopper([&] {
            while (!early_done.load(std::memory_order_acquire)) std::this_thread::yield();
            try { group.close(); } catch (...) { stop_error = std::current_exception(); }
        });
        std::thread callback([&] {
            while (!final_release.load(std::memory_order_acquire)) std::this_thread::yield();
            try { registry->release_callback(held); }
            catch (...) { release_error = std::current_exception(); }
        });
        stopper.join();
        // Capture observations before releasing the held pin; defer failure
        // reporting until all workers join so no test thread outlives storage.
        const bool pinned = owned.poll() == Poll::pending && freed.load() == 0
            && ref.get_done().wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout;
        final_release.store(true, std::memory_order_release);
        bool valid_poll = true;
        for (unsigned i = 0; i < 256; ++i) {
            const auto result = owned.poll();
            valid_poll = valid_poll && (result == Poll::pending || result == Poll::completed);
            std::this_thread::yield();
        }
        attacker.join();
        callback.join();
        for (const auto& error : {attacker_error, release_error, stop_error})
            if (error) std::rethrow_exception(error);
        require(pinned && valid_poll && rejected >= 192, "retirement race lost its pin or accepted a foreign ticket");
        require(owned.poll() == Poll::completed && freed.load() == 1, "racing callback did not reclaim exactly once");
        ref.get_done().get();
        require(owned.poll() == Poll::completed && freed.load() == 1, "racing done reclaimed twice");
        foreign.release_callback(other);
        foreign.retire();
        require(foreign.try_reclaim().status == ActorResourceRegistry::Reclaim::complete, "foreign callback was consumed");
    }
}

static void deferred_source_cleanup_physically_fences_actor_done() {
    NativeActorResourceQuarantine quarantine(1);
    NativeActorGroup group(410, 1);
    auto registry = std::make_shared<ActorResourceRegistry>(17, 1);
    auto receipt = std::make_shared<NativeSourceCleanupGate>(
            NativeSourceCleanupTicket{17, 1, 99});
    const NativeSourceCleanupTicket ticket{17, 1, 99};
    std::atomic<int> freed{0};
    registry->register_resource(Kind::native_allocation, [&] { ++freed; });
    // A second suspended child pins the actor even after this one finishes.
    const auto outstanding_child = registry->acquire_loan();
    NativeActorRetirementBarrier* barrier = nullptr;
    auto ref = group.adopt(410, 17, 1, [&] { barrier->retire(); });
    bool wrong_incarnation_denied = false;
    try {
        auto stale = std::make_shared<NativeSourceCleanupGate>(
                NativeSourceCleanupTicket{17, 2, 99});
        NativeActorRetirementBarrier invalid(registry, group.runtime_finalizer(ref),
                                             quarantine, stale);
    } catch (const std::invalid_argument&) {
        wrong_incarnation_denied = true;
    }
    require(wrong_incarnation_denied, "foreign cleanup receipt admitted");
    NativeActorRetirementBarrier owned(registry, group.runtime_finalizer(ref),
                                       quarantine, receipt);
    barrier = &owned;
    require(group.mark_ready(ref), "cleanup proof actor ready");
    group.close();
    require(owned.poll() == Poll::pending && freed == 0,
            "scheduler close without cleanup proof freed arena");
    require(receipt->request_cancel(ticket), "cancel request");
    require(owned.poll() == Poll::pending, "cancel request faked physical done");
    require(NativeSourceCleanupGateTestPeer::claim(*receipt, ticket), "cleanup turn claim");
    require(!NativeSourceCleanupGateTestPeer::claim(*receipt, ticket), "duplicate cleanup turn claim");
    require(owned.poll() == Poll::pending, "incomplete cleanup turn freed arena");
    require(NativeSourceCleanupGateTestPeer::yield(*receipt, ticket), "cleanup await yield");
    require(owned.poll() == Poll::pending, "yielded cleanup freed arena");
    require(NativeSourceCleanupGateTestPeer::claim(*receipt, ticket), "cleanup resume on owner lane");
    require(NativeSourceCleanupGateTestPeer::complete(*receipt, ticket), "cleanup completion");
    require(owned.poll() == Poll::pending && freed == 0,
            "another suspended continuation lost its physical lifetime pin");
    registry->release_loan(outstanding_child);
    require(owned.poll() == Poll::completed && freed == 1,
            "physically completed cleanup failed to enable actor retirement");
    ref.get_done().get();
    require(!receipt->request_cancel(ticket), "late cancel after physical done");
}

// No synthetic native-allocation callback here: a deliberately incomplete
// cleanup must remain unreclaimed, and a sanitizing test must not leak a
// fake arena during process teardown. The success test checks real closers.
static void failed_source_cleanup_keeps_actor_arena_retained() {
    NativeActorResourceQuarantine quarantine(1);
    NativeActorGroup group(411, 1);
    auto registry = std::make_shared<ActorResourceRegistry>(18, 1);
    auto receipt = std::make_shared<NativeSourceCleanupGate>(
            NativeSourceCleanupTicket{18, 1, 102});
    const NativeSourceCleanupTicket ticket{18, 1, 102};
    NativeActorRetirementBarrier* barrier = nullptr;
    auto ref = group.adopt(411, 18, 1, [&] { barrier->retire(); });
    NativeActorRetirementBarrier owned(registry, group.runtime_finalizer(ref),
                                       quarantine, receipt);
    barrier = &owned;
    require(group.mark_ready(ref), "failed cleanup actor ready");
    group.close();
    require(receipt->request_cancel(ticket), "failed cleanup cancel request");
    require(NativeSourceCleanupGateTestPeer::claim(*receipt, ticket), "failed cleanup claim");
    require(receipt->fail_cleanup(ticket), "failed cleanup marker");
    require(!NativeSourceCleanupGateTestPeer::complete(*receipt, ticket), "failed cleanup claimed completion");
    require(owned.poll() == Poll::cleanup_incomplete
                && registry->phase() == ActorResourceRegistry::Phase::retiring,
            "failed cleanup reclaimed actor resources");
    require(ref.get_done().wait_for(std::chrono::milliseconds(0))
                 == std::future_status::timeout,
            "missing cleanup created a false get_done");
}

static void two_overlapping_receives_pin_actual_native_done_barrier() {
    NativeActorResourceQuarantine quarantine(1);
    NativeActorGroup group(412, 1);
    auto registry = std::make_shared<ActorResourceRegistry>(19, 1);
    auto receives = std::make_shared<NativeActorReceiveFence>(19, 1, 2);
    std::atomic<int> physical_frees{0};
    registry->register_resource(Kind::native_allocation, [&] {
        ++physical_frees;
    });

    NativeActorRetirementBarrier* barrier = nullptr;
    auto ref = group.adopt(412, 19, 1, [&] { barrier->retire(); });
    NativeActorRetirementBarrier owned(
            registry, group.runtime_finalizer(ref), quarantine, {}, receives);
    barrier = &owned;
    require(group.mark_ready(ref), "two-receive actor ready");

    auto first = receives->try_enter(19, 1);
    auto second = receives->try_enter(19, 1);
    require(first.has_value() && second.has_value(), "two distinct admissions");
    require(!receives->try_enter(19, 1), "invocation capacity bypass");
    require(receives->active_receives() == 2, "receive count");
    group.close();
    receives->seal_stop(true);
    require(!receives->try_enter(19, 1), "receive after STOP");
    require(!receives->try_begin_teardown(), "teardown while both receives live");
    require(owned.poll() == Poll::pending && physical_frees == 0,
            "retirement released arena while two receives were live");
    require(ref.get_done().wait_for(std::chrono::milliseconds(0))
                 == std::future_status::timeout, "done before receive quiescence");

    first->release();
    require(!receives->try_begin_teardown(), "teardown while second receive live");
    require(owned.poll() == Poll::pending && physical_frees == 0,
            "retirement released arena while second receive live");
    second->release();
    first->release(); // repeated close is inert
    require(receives->active_receives() == 0, "receive leases did not retire");
    require(receives->try_begin_teardown(), "once-only lifecycle reservation");
    require(!receives->try_begin_teardown(), "duplicate lifecycle reservation");
    require(owned.poll() == Poll::pending && physical_frees == 0,
            "resource free overlapped running teardown");
    receives->seal_stop(false); // cancel cannot interrupt a live native callback
    require(!receives->ready_for_reclamation(),
            "abortive stop bypassed already-entered teardown");
    receives->finish_teardown();
    require(owned.poll() == Poll::completed && physical_frees == 1,
            "native resource was not reclaimed exactly once after quiescence");
    ref.get_done().get();
    require(owned.poll() == Poll::completed && physical_frees == 1,
            "second DONE reclaimed native resource twice");
}

static void foreign_receive_fence_cannot_complete_another_actor() {
    NativeActorResourceQuarantine quarantine(1);
    NativeActorGroup group(413, 1);
    auto registry = std::make_shared<ActorResourceRegistry>(20, 5);
    auto foreign = std::make_shared<NativeActorReceiveFence>(20, 6, 1);
    NativeActorRetirementBarrier* barrier = nullptr;
    auto ref = group.adopt(413, 20, 5, [&] { barrier->retire(); });
    bool rejected = false;
    try {
        NativeActorRetirementBarrier invalid(
                registry, group.runtime_finalizer(ref), quarantine, {}, foreign);
    } catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "foreign incarnation receive fence accepted");
    auto correct = std::make_shared<NativeActorReceiveFence>(20, 5, 1);
    NativeActorRetirementBarrier owned(
            registry, group.runtime_finalizer(ref), quarantine, {}, correct);
    barrier = &owned;
    require(group.mark_ready(ref), "correct incarnation ready");
    rejected = false;
    try { (void) correct->try_enter(20, 4); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected && correct->active_receives() == 0,
            "stale generation minted guest receive lease");
    group.close();
    require(owned.poll() == Poll::pending,
            "missing stop-seal proof admitted native reclamation");
    correct->seal_stop(false);
    require(owned.poll() == Poll::completed, "abortive stop did not retire resources");
    ref.get_done().get();
}

// Concurrent host admission/STOP race for the new bounded invocation gate.
// This is deliberately NOT a compiled .atto concurrent receive fixture.
static void native_receive_stop_admission_race_is_linearizable() {
    for (unsigned round = 0; round < 64; ++round) {
        auto receives = std::make_shared<NativeActorReceiveFence>(91, round + 1, 2);
        std::atomic<bool> start{false}, release{false};
        std::atomic<unsigned> attempted{0}, accepted{0};
        auto worker = [&] {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            auto lease = receives->try_enter(91, round + 1);
            if (lease) accepted.fetch_add(1, std::memory_order_release);
            attempted.fetch_add(1, std::memory_order_release);
            while (!release.load(std::memory_order_acquire))
                std::this_thread::yield();
        };
        std::thread a(worker), b(worker);
        start.store(true, std::memory_order_release);
        // The winning STOP may race with either receive reservation.
        receives->seal_stop(false);
        while (attempted.load(std::memory_order_acquire) != 2)
            std::this_thread::yield();
        const auto count = accepted.load(std::memory_order_acquire);
        const bool valid = count <= 2 && receives->active_receives() == count
                && !receives->try_enter(91, round + 1)
                && (count != 0 || receives->ready_for_reclamation())
                && (count == 0 || !receives->ready_for_reclamation());
        release.store(true, std::memory_order_release);
        a.join();
        b.join();
        require(valid, "STOP race admitted wrong or unaccounted receive");
        require(receives->active_receives() == 0
                    && receives->ready_for_reclamation(),
                "STOP race stranded receive lease or physical retirement");
    }
}

static void invalid_receive_fence_construction_and_post_owner_release() {
    bool rejected_zero_actor = false;
    bool rejected_zero_incarnation = false;
    bool rejected_zero_capacity = false;
    bool rejected_excessive_capacity = false;
    try { NativeActorReceiveFence invalid(0, 1, 1); }
    catch (const std::invalid_argument&) { rejected_zero_actor = true; }
    try { NativeActorReceiveFence invalid(1, 0, 1); }
    catch (const std::invalid_argument&) { rejected_zero_incarnation = true; }
    try { NativeActorReceiveFence invalid(1, 1, 0); }
    catch (const std::invalid_argument&) { rejected_zero_capacity = true; }
    try { NativeActorReceiveFence invalid(1, 1, 4097); }
    catch (const std::invalid_argument&) { rejected_excessive_capacity = true; }
    require(rejected_zero_actor && rejected_zero_incarnation
                && rejected_zero_capacity && rejected_excessive_capacity,
            "invalid native actor receive admission allocated a fence");

    std::optional<NativeActorReceiveFence::ReceiveLease> outstanding;
    {
        auto fence = std::make_unique<NativeActorReceiveFence>(22, 9, 1);
        outstanding = fence->try_enter(22, 9);
        require(outstanding.has_value(), "receive lease admission");
    }
    // The lease owns the bounded generation state, not a raw pointer to its
    // now-destroyed factory gate. Dropping it cannot use freed gate memory.
    outstanding.reset();
}

// Two physically separate host carriers keep a native actor's invocation
// pins alive after its group is detached. This is actual concurrent host
// receipt execution, NOT two compiled .atto receiver stacks.
static void concurrent_host_receivers_join_native_resource_retirement() {
    NativeActorResourceQuarantine quarantine(1);
    NativeActorGroup group(414, 1);
    auto registry = std::make_shared<ActorResourceRegistry>(23, 4);
    auto receives = std::make_shared<NativeActorReceiveFence>(23, 4, 2);
    // Allocate an actual actor-owned native heap, not only a mock closer.
    // The scalar ABI remains serial; the concurrent threads below hold
    // invocation receipts but deliberately do not enter that scalar heap.
    OresActorI64Heap* heap = ores_actor_i64_heap_open(23, 4, 4);
    require(heap != nullptr, "parallel retirement native arena admission");
    const auto heap_turn = ores_actor_i64_heap_enter(heap, 23, 4);
    const auto heap_slot = ores_actor_i64_heap_new(heap, heap_turn, 42);
    require(ores_actor_i64_heap_read(heap, heap_turn, heap_slot) == 42,
            "parallel retirement arena did not preserve owned state");
    ores_actor_i64_heap_leave(heap, heap_turn);
    std::atomic<unsigned> arrived{0};
    std::atomic<bool> release_a{false}, release_b{false};
    std::atomic<int> freed{0};
    registry->register_resource(Kind::native_allocation, [heap, &freed] {
        if (!ores_actor_i64_heap_try_destroy(heap)) {
            throw std::runtime_error("parallel receive native arena remained pinned");
        }
        freed.fetch_add(1, std::memory_order_relaxed);
    });

    NativeActorRetirementBarrier* retiring = nullptr;
    auto ref = group.adopt(414, 23, 4, [&] {
        ores_actor_i64_heap_retire(heap);
        retiring->retire();
    });
    NativeActorRetirementBarrier barrier(
        registry, group.runtime_finalizer(ref), quarantine, {}, receives);
    retiring = &barrier;
    require(group.mark_ready(ref), "parallel host receive actor ready");

    std::exception_ptr failure_a, failure_b;
    auto worker = [&](std::atomic<bool>& release, std::exception_ptr& failure) {
        try {
            auto lease = receives->try_enter(23, 4);
            if (!lease) throw std::logic_error("receive capacity unexpectedly closed");
            arrived.fetch_add(1, std::memory_order_release);
            while (!release.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        } catch (...) {
            failure = std::current_exception();
            arrived.fetch_add(1, std::memory_order_release);
        }
    };
    std::thread a(worker, std::ref(release_a), std::ref(failure_a));
    std::thread b(worker, std::ref(release_b), std::ref(failure_b));
    const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::seconds(5);
    while (arrived.load(std::memory_order_acquire) != 2
            && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }

    bool before = false, between = false, after = false;
    const bool admitted_both = arrived.load(std::memory_order_acquire) == 2;
    std::exception_ptr lifecycle_failure;
    if (admitted_both) {
        try {
            group.close();
            receives->seal_stop(true);
            before = receives->active_receives() == 2
                && !receives->try_enter(23, 4)
                && !receives->try_begin_teardown()
                && barrier.poll() == Poll::pending
                && freed.load(std::memory_order_relaxed) == 0
                && ref.get_done().wait_for(std::chrono::milliseconds(0))
                    == std::future_status::timeout;
        } catch (...) {
            lifecycle_failure = std::current_exception();
        }
    }

    // Never throw before joining either worker. A failing native STOP or
    // native closer must fail the test rather than calling std::terminate()
    // when a still-joinable worker's destructor runs during stack unwind.
    release_a.store(true, std::memory_order_release);
    a.join();
    if (admitted_both && !lifecycle_failure) {
        try {
            between = receives->active_receives() == 1
                && !receives->try_begin_teardown()
                && barrier.poll() == Poll::pending
                && freed.load(std::memory_order_relaxed) == 0;
        } catch (...) {
            lifecycle_failure = std::current_exception();
        }
    }
    release_b.store(true, std::memory_order_release);
    b.join();
    if (admitted_both && !lifecycle_failure) {
        try {
            after = receives->active_receives() == 0
                && receives->try_begin_teardown()
                && barrier.poll() == Poll::pending
                && freed.load(std::memory_order_relaxed) == 0;
        } catch (...) {
            lifecycle_failure = std::current_exception();
        }
    }

    if (lifecycle_failure) std::rethrow_exception(lifecycle_failure);
    if (failure_a) std::rethrow_exception(failure_a);
    if (failure_b) std::rethrow_exception(failure_b);
    require(before && between && after,
            "two host carriers did not preserve native actor retirement pins");
    receives->finish_teardown();
    require(barrier.poll() == Poll::completed,
            "resource reclamation did not follow both receiver exits and teardown");
    require(freed.load(std::memory_order_relaxed) == 1,
            "native actor resource closer did not run exactly once");
    ref.get_done().get();
}

// Host-only serial admission contract. The compiler must eventually select
// capacity=1 for 'define serial actor' and a bounded >1 value for default actors.
// A suspended serial invocation retains its lease even without a carrier.
static void serial_actor_receive_capacity_retains_suspended_identity() {
    auto serial = std::make_shared<NativeActorReceiveFence>(24, 7, 1);
    auto first = serial->try_enter(24, 7);
    require(first.has_value(), "serial first receive admission");
    require(!serial->try_enter(24, 7), "serial actor admitted second live receive");
    // Suspending the invocation cannot release its logical stack lease.
    // A second DATA envelope must remain queued until physical completion.
    require(serial->active_receives() == 1, "serial suspended stack lost its pin");
    first->release();
    auto next = serial->try_enter(24, 7);
    require(next.has_value(), "serial actor failed to admit next receive after completion");
    serial->seal_stop(true);
    require(!serial->try_enter(24, 7), "serial stopped actor accepted user DATA");
    require(!serial->try_begin_teardown(), "serial active receive overlapped teardown");
    next->release();
    require(serial->try_begin_teardown(), "serial cleanup never entered");
    serial->finish_teardown();
    require(serial->ready_for_reclamation(), "serial incarnation did not quiesce");
}

int main() {
    serial_actor_receive_capacity_retains_suspended_identity();
    concurrent_host_receivers_join_native_resource_retirement();
    invalid_receive_fence_construction_and_post_owner_release();
    native_receive_stop_admission_race_is_linearizable();
    two_overlapping_receives_pin_actual_native_done_barrier();
    foreign_receive_fence_cannot_complete_another_actor();
    deferred_source_cleanup_physically_fences_actor_done();
    failed_source_cleanup_keeps_actor_arena_retained();
    successful_heap_retirement();
    failed_ffi_revocation_is_quarantined();
    quarantine_saturation_does_not_settle_done();
    done_waits_for_group_detachment();
    physical_reclamation_waits_for_admitted_actor_turn();
    foreign_registry_cannot_release_live_heap_callback();
    foreign_release_races_with_retirement_and_last_callback();
}

