#include "oreslang/runtime/ActorGroupLifecycle.hpp"
#include "oreslang/runtime/ActorOwnedI64Heap.h"
#include "oreslang/runtime/NativeActorRetirementBarrier.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

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
    registry->bind_owner(ref);
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
    registry->bind_owner(ref);
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
    first->bind_owner(r1);
    auto r2 = group.adopt(303, 10, 1, [&] { b2->retire(); });
    second->bind_owner(r2);
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
    registry->bind_owner(ref);
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
    registry->bind_owner(ref);
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
    registry->bind_owner(ref);
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
    registry->bind_owner(ref);
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
    registry->bind_owner(ref);
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
    registry->bind_owner(ref);
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
    auto ref = group.adopt(412, 19, 1, [&] { barrier->retire(true); });
    registry->bind_owner(ref);
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
    // group.close's callback itself must seal receives: a separate seal_stop
    // call would conceal the dangerous retirement-versus-admission window.
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

// A resource registry for actor A must never be coupled to actor B's DONE
// capability, even if that foreign group has already completed shutdown.
// Runtime ID is not derivable from actor ID and generation. Independently
// created runtimes may both own actor 52/generation 7, so a finalizer with
// those same numbers must still be rejected unless the registry explicitly
// binds the originating runtime domain.
// Exact owner-ticket checks distinguish independent runtimes and actors
// that reuse the same (runtime, actor, generation) tuple after physical DONE.
static void native_barrier_refuses_unbound_or_wrong_domain_finalizers() {
    NativeActorResourceQuarantine quarantine(1);

    {
        NativeActorGroup group(420, 1);
        auto owner = group.adopt(420, 52, 7, [] {});
        require(group.mark_ready(owner), "unbound finalizer actor ready");
        auto registry = std::make_shared<ActorResourceRegistry>(52, 7);
        require(!registry->has_bound_owner(), "fresh resource registry already bound");
        bool rejected = false;
        try {
            NativeActorRetirementBarrier invalid(
                registry, group.runtime_finalizer(owner), quarantine);
        } catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "unbound registry minted physical DONE authority");
        group.close();
        require(group.runtime_finalizer(owner).finalized(),
                "unbound test unrelated actor never finished");
        owner.get_done().get();
    }

    {
        NativeActorGroup group(421, 2);
        auto owner = group.adopt(421, 52, 7, [] {});
        auto foreignId = group.adopt(421, 53, 7, [] {});
        require(group.mark_ready(owner) && group.mark_ready(foreignId),
                "two actors in the same runtime ready");
        auto registry = std::make_shared<ActorResourceRegistry>(52, 7);
        registry->bind_owner(owner);
        bool rejected = false;
        try {
            NativeActorRetirementBarrier invalid(
                registry, group.runtime_finalizer(foreignId), quarantine);
        } catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "foreign actor ID passed exact-ticket retirement");
        NativeActorRetirementBarrier valid(
            registry, group.runtime_finalizer(owner), quarantine);
        group.close();
        valid.retire();
        require(valid.poll() == Poll::completed,
                "owning actor finalizer was not accepted");
        require(group.runtime_finalizer(foreignId).finalized(),
                "unrelated actor did not independently finalize");
        owner.get_done().get();
        foreignId.get_done().get();
    }

    {
        NativeActorGroup group(422, 1);
        auto wrongGeneration = group.adopt(422, 52, 8, [] {});
        require(group.mark_ready(wrongGeneration), "foreign generation ready");
        auto registry = std::make_shared<ActorResourceRegistry>(52, 7);
        bool rejected = false;
        try { registry->bind_owner(wrongGeneration); }
        catch (const std::invalid_argument&) { rejected = true; }
        require(rejected && !registry->has_bound_owner(),
                "wrong incarnation minted an owner ticket");
        group.close();
        require(group.runtime_finalizer(wrongGeneration).finalized(),
                "wrong generation did not independently finalize");
        wrongGeneration.get_done().get();
    }

    {
        NativeActorGroup ownerGroup(423, 1);
        NativeActorGroup foreignGroup(424, 1);
        auto owner = ownerGroup.adopt(423, 52, 7, [] {});
        auto foreign = foreignGroup.adopt(424, 52, 7, [] {});
        require(ownerGroup.mark_ready(owner) && foreignGroup.mark_ready(foreign),
                "same-numeric actors in distinct runtimes ready");
        auto registry = std::make_shared<ActorResourceRegistry>(52, 7);
        registry->bind_owner(owner);
        bool rejected = false;
        try {
            NativeActorRetirementBarrier invalid(
                registry, foreignGroup.runtime_finalizer(foreign), quarantine);
        } catch (const std::invalid_argument&) { rejected = true; }
        require(rejected,
                "foreign runtime with matching actor and generation passed");
        NativeActorRetirementBarrier valid(
            registry, ownerGroup.runtime_finalizer(owner), quarantine);
        ownerGroup.close();
        foreignGroup.close();
        valid.retire();
        require(valid.poll() == Poll::completed,
                "real owner failed physical retirement");
        require(foreignGroup.runtime_finalizer(foreign).finalized(),
                "foreign runtime leaked after rejected finalizer");
        owner.get_done().get();
        foreign.get_done().get();
    }

    {
        NativeActorGroup priorGroup(425, 1);
        auto prior = priorGroup.adopt(425, 52, 7, [] {});
        require(priorGroup.mark_ready(prior), "prior actor ready");
        auto registry = std::make_shared<ActorResourceRegistry>(52, 7);
        registry->bind_owner(prior);
        bool duplicateRejected = false;
        try { registry->bind_owner(prior); }
        catch (const std::logic_error&) { duplicateRejected = true; }
        require(duplicateRejected, "registry accepted duplicate owner binding");
        priorGroup.close();
        require(priorGroup.runtime_finalizer(prior).finalized(),
                "old incarnation did not release global owner record");
        prior.get_done().get();

        // Same runtime, actor ID and generation, but a new opaque ticket:
        // the old registry must NEVER authorize the new actor's DONE.
        NativeActorGroup newGroup(425, 1);
        auto fresh = newGroup.adopt(425, 52, 7, [] {});
        require(newGroup.mark_ready(fresh), "recreated actor ready");
        require(!prior.same_incarnation(fresh),
                "replacement actor reused the old opaque ticket");
        bool staleRejected = false;
        try {
            NativeActorRetirementBarrier invalid(
                registry, newGroup.runtime_finalizer(fresh), quarantine);
        } catch (const std::invalid_argument&) { staleRejected = true; }
        require(staleRejected,
                "stale registry acknowledged replacement actor with same tuple");
        newGroup.close();
        require(newGroup.runtime_finalizer(fresh).finalized(),
                "replacement actor failed independent finalization");
        fresh.get_done().get();
    }
}

static void foreign_finalizer_cannot_acknowledge_another_actor_registry() {
    NativeActorResourceQuarantine quarantine(1);
    NativeActorGroup owningGroup(417, 1);
    auto owner = owningGroup.adopt(417, 52, 7, [] {});
    require(owningGroup.mark_ready(owner), "owned resource actor ready");
    auto registry = std::make_shared<ActorResourceRegistry>(52, 7);
    registry->bind_owner(owner);

    NativeActorGroup otherActorGroup(418, 1);
    auto otherActor = otherActorGroup.adopt(418, 53, 7, [] {});
    require(otherActorGroup.mark_ready(otherActor), "foreign actor ready");
    bool foreignIdRejected = false;
    try {
        NativeActorRetirementBarrier invalid(
            registry, otherActorGroup.runtime_finalizer(otherActor), quarantine);
    } catch (const std::invalid_argument&) { foreignIdRejected = true; }
    require(foreignIdRejected,
            "native retirement barrier accepted finalizer from foreign actor");

    NativeActorGroup otherIncarnationGroup(419, 1);
    auto otherIncarnation = otherIncarnationGroup.adopt(419, 52, 8, [] {});
    require(otherIncarnationGroup.mark_ready(otherIncarnation),
            "foreign incarnation ready");
    bool foreignGenerationRejected = false;
    try {
        NativeActorRetirementBarrier invalid(
            registry, otherIncarnationGroup.runtime_finalizer(otherIncarnation),
            quarantine);
    } catch (const std::invalid_argument&) { foreignGenerationRejected = true; }
    require(foreignGenerationRejected,
            "native retirement barrier accepted finalizer from stale incarnation");

    // All three actor groups retain independent physical DONE capabilities.
    NativeActorRetirementBarrier valid(
        registry, owningGroup.runtime_finalizer(owner), quarantine);
    owningGroup.close();
    otherActorGroup.close();
    otherIncarnationGroup.close();
    valid.retire();
    require(valid.poll() == Poll::completed,
            "owning actor failed registry reclamation");
    require(otherActorGroup.runtime_finalizer(otherActor).finalized(),
            "unrelated actor failed physical finalization");
    require(otherIncarnationGroup.runtime_finalizer(otherIncarnation).finalized(),
            "unrelated incarnation failed physical finalization");
    owner.get_done().get();
    otherActor.get_done().get();
    otherIncarnation.get_done().get();
}

static void foreign_receive_fence_cannot_complete_another_actor() {
    NativeActorResourceQuarantine quarantine(1);
    NativeActorGroup group(413, 1);
    auto registry = std::make_shared<ActorResourceRegistry>(20, 5);
    auto foreign = std::make_shared<NativeActorReceiveFence>(20, 6, 1);
    NativeActorRetirementBarrier* barrier = nullptr;
    auto ref = group.adopt(413, 20, 5, [&] { barrier->retire(); });
    registry->bind_owner(ref);
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
    require(!correct->try_enter(20, 5),
            "abortive group STOP returned with guest receive admission open");
    require(owned.poll() == Poll::completed,
            "abortive stop failed to seal and reclaim native resources");
    ref.get_done().get();
}

// Concurrent host admission/STOP race for the new bounded invocation gate.
// This is deliberately NOT a compiled .atto concurrent receive fixture.
static void native_receive_stop_admission_race_is_linearizable() {
    for (unsigned round = 0; round < 256; ++round) {
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
        // Seal before ANY arena/IO retirement work. retire(true) repeats
        // the seal as a fail-closed last defense before registry retirement.
        retiring->seal_stop(true);
    registry->bind_owner(ref);
        ores_actor_i64_heap_retire(heap);
        retiring->retire(true);
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
            // STOP must have sealed new receive admission before return.
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

// The group-close retirement callback must seal DATA admission itself.
// An additional explicit caller-side seal_stop would hide a gap where
// the native resource registry was retired while new receive leases entered.
static void abortive_group_stop_seals_receive_before_any_native_reclaim() {
    NativeActorResourceQuarantine quarantine(1);
    NativeActorGroup group(415, 1);
    auto registry = std::make_shared<ActorResourceRegistry>(25, 2);
    auto receives = std::make_shared<NativeActorReceiveFence>(25, 2, 2);
    std::atomic<unsigned> released{0};
    registry->register_resource(Kind::native_allocation, [&] {
        released.fetch_add(1, std::memory_order_relaxed);
    });
    NativeActorRetirementBarrier* retiring = nullptr;
    auto ref = group.adopt(415, 25, 2, [&] { retiring->retire(); });
    registry->bind_owner(ref);
    NativeActorRetirementBarrier barrier(
        registry, group.runtime_finalizer(ref), quarantine, {}, receives);
    retiring = &barrier;
    require(group.mark_ready(ref), "abortive actor ready");
    auto first = receives->try_enter(25, 2);
    require(first.has_value(), "abortive actor first receive");
    group.close(); // callback's retire() must seal the gate as terminal.
    require(!receives->try_enter(25, 2),
            "abortive group STOP admitted DATA after retirement began");
    require(!receives->try_begin_teardown(),
            "abortive STOP allowed guest tear_down");
    require(barrier.poll() == Poll::pending,
            "abortive STOP ignored still-live receive");
    require(ref.get_done().wait_for(std::chrono::milliseconds(0))
                == std::future_status::timeout,
            "abortive STOP settled DONE while guest receive was live");
    require(released.load(std::memory_order_relaxed) == 0,
            "abortive STOP freed live receiver memory");
    first->release();
    require(receives->ready_for_reclamation(),
            "abortive STOP could not reclaim after receive exit");
    require(barrier.poll() == Poll::completed,
            "abortive STOP failed to finalize after physical quiescence");
    require(released.load(std::memory_order_relaxed) == 1,
            "abortive STOP released native resource more than once");
    ref.get_done().get();
    require(!receives->try_begin_teardown(),
            "failed guest lifecycle teardown appeared after DONE");
}

// A future concurrent actor must not bypass capacity bounds or leak an
// actor-owned pin when a logical invocation lease moves between stacks.
static void native_receive_capacity_move_and_owner_death() {
    NativeActorReceiveFence bounded(82, 1, 4096);
    std::vector<NativeActorReceiveFence::ReceiveLease> held;
    held.reserve(4096);
    for (unsigned i = 0; i < 4096; ++i) {
        auto next = bounded.try_enter(82, 1);
        require(next.has_value(), "bounded receive capacity lost before 4096");
        held.push_back(std::move(*next));
    }
    require(bounded.active_receives() == 4096
                && !bounded.try_enter(82, 1),
            "native receive capacity overflow");
    bool foreignRejected = false;
    try { (void) bounded.try_enter(82, 2); }
    catch (const std::invalid_argument&) { foreignRejected = true; }
    require(foreignRejected && bounded.active_receives() == 4096,
            "foreign generation changed active receive count");
    bounded.seal_stop(true);
    require(!bounded.try_begin_teardown(), "teardown entered while capacity fully leased");
    held.clear();
    require(bounded.active_receives() == 0,
            "bulk move-only lease destruction did not drain");
    require(bounded.try_begin_teardown(), "graceful teardown never became eligible");
    bounded.seal_stop(false);
    require(!bounded.ready_for_reclamation(),
            "abortive downgrade bypassed live teardown");
    bounded.finish_teardown();
    require(bounded.ready_for_reclamation(),
            "finished teardown did not authorize safe reclamation");

    NativeActorReceiveFence from(83, 1, 2);
    NativeActorReceiveFence to(84, 1, 2);
    auto first = from.try_enter(83, 1);
    auto second = to.try_enter(84, 1);
    require(first.has_value() && second.has_value(),
            "move-assignment lease setup failed");
    *first = std::move(*second);
    require(from.active_receives() == 0 && to.active_receives() == 1,
            "move-assignment leaked previous actor's receive pin");
    from.seal_stop(false);
    to.seal_stop(false);
    require(from.ready_for_reclamation(), "old actor still pinned by moved lease");
    first.reset();
    second.reset();
    require(to.ready_for_reclamation(), "new actor still pinned after moved lease drop");

    std::optional<NativeActorReceiveFence::ReceiveLease> postOwner;
    {
        auto owner = std::make_unique<NativeActorReceiveFence>(85, 1, 1);
        postOwner = owner->try_enter(85, 1);
        require(postOwner.has_value(), "post-owner gate lease admission");
        owner->seal_stop(false);
    }
    // The receipt holds shared State, not the destroyed factory object's
    // address. This drop is safe even after the gate owner was released.
    postOwner.reset();
}

int main() {
    native_receive_capacity_move_and_owner_death();
    native_barrier_refuses_unbound_or_wrong_domain_finalizers();
    foreign_finalizer_cannot_acknowledge_another_actor_registry();
    abortive_group_stop_seals_receive_before_any_native_reclaim();
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

