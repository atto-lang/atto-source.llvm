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
    // Mock FFI producers are stopped; host has verified this test's isolation
    // boundary and can explicitly drop quarantined captures. The second
    // registry remains owned by the live barrier until the test exits.
    quarantine.release_after_verified_isolate_shutdown();
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

int main() {
    successful_heap_retirement();
    failed_ffi_revocation_is_quarantined();
    quarantine_saturation_does_not_settle_done();
    done_waits_for_group_detachment();
}
