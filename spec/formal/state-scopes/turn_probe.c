// Include the production C implementation to exercise its actual private
// require_turn predicate, rather than reimplementing that predicate here.
#include "oreslang/runtime/ActorOwnedI64Heap.c"

int turn_admits(uint64_t live_domain, uint64_t live_epoch,
                uint64_t requested_domain, uint64_t requested_epoch,
                int active, int retiring) {
    OresActorI64Heap heap = {0};
    heap.domain = live_domain;
    heap.turn_epoch = live_epoch;
    heap.turn_active = active;
    heap.retiring = retiring;
    const OresActorTurn turn = {requested_domain, requested_epoch};
    require_turn(&heap, turn);
    return 1;
}
