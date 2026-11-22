#include "oreslang/runtime/ActorGroupLifecycle.hpp"
#include "oreslang/runtime/ActorOwnedI64Heap.h"

#include <cstdint>
#include <chrono>
#include <future>
#include <stdexcept>

using oreslang::runtime::NativeActorGroup;

static void require(bool value, const char* why) {
    if (!value) throw std::runtime_error(why);
}

int main() {
    // The ActorGroup owns a small actor ref, never the native heap pointer.
    // A supervisor's stop callback retires the native allocation domain;
    // physical release is held back until outstanding cross-turn loans end.
    constexpr std::uint64_t actor = 7, incarnation = 3;
    OresActorI64Heap* heap = ores_actor_i64_heap_open(actor, incarnation, 4);
    require(heap != nullptr, "could not open native actor heap");
    OresActorTurn turn = ores_actor_i64_heap_enter(heap, actor, incarnation);
    OresActorSlot slot = ores_actor_i64_heap_new(heap, turn, 123);
    OresActorLoan suspended_loan = ores_actor_i64_heap_borrow(heap, turn, slot);
    ores_actor_i64_heap_leave(heap, turn);

    int stop_requests = 0;
    NativeActorGroup group(100, 2);
    auto ref = group.adopt(100, actor, incarnation, [&] {
        ++stop_requests;
        ores_actor_i64_heap_retire(heap);
    });
    require(group.mark_ready(ref), "actor startup not admitted");
    group.close();
    require(group.closed() && group.worker_count() == 0,
            "supervisor retained actor after close");
    require(ref.get_done().wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout,
            "close fabricated done before native heap loan quiescence");
    require(stop_requests == 1, "supervisor did not request native retirement");
    require(!group.mark_ready(ref), "closed supervisor admitted stale readiness");
    require(ores_actor_i64_heap_try_destroy(heap) == 0,
            "native heap reclaimed while a suspended loan still pins memory");

    ores_actor_i64_heap_release(heap, suspended_loan);
    require(ores_actor_i64_heap_try_destroy(heap) == 1,
            "native actor heap not reclaimable after last loan");
    require(ref.get_done().wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout,
            "physical free did not require supervisor finalization acknowledgement");
    require(group.finalized(ref), "supervisor cannot acknowledge physically freed heap");
    ref.get_done().get();
    require(!group.finalized(ref), "duplicate physical finalization accepted");
    group.close();
    require(stop_requests == 1, "group requested duplicate actor teardown");
}
