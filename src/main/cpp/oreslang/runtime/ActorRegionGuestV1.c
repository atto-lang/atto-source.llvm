#include "oreslang/runtime/ActorRegionGuestV1.h"
#include "oreslang/runtime/ActorRegionV1.h"
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#define ORES_GUEST_REGION_MAX_SLOTS 4096u
#define ORES_GUEST_REGION_MAX_SLOT_BYTES (1024u * 1024u)
#define ORES_GUEST_REGION_MAX_SCRATCH_BYTES (16u * 1024u * 1024u)

typedef struct {
    OresActorRegionV1 *region;
    OresArenaTurn turn;
    uint32_t slots;
    uint32_t stride;
    uint32_t live_count;
    uint32_t next_guest_slot;
    OresActorRegionV1Ref refs[ORES_GUEST_REGION_MAX_SLOTS];
    unsigned char live[ORES_GUEST_REGION_MAX_SLOTS];
} GuestScope;

/* Synchronous actor-main profile is physically pinned to its entry thread.
 * No task, continuation, callback or actor migration is admitted in v1. */
static _Thread_local GuestScope guest_scope;
static _Atomic(uint64_t) next_guest_identity = 1;

static void deny_guest_bridge(void) { abort(); }

static void require_scope(void) {
    if (!guest_scope.region || !guest_scope.slots || !guest_scope.stride)
        deny_guest_bridge();
}

static uint32_t checked_index(uint32_t offset) {
    require_scope();
    if (!offset) deny_guest_bridge();
    const uint32_t zero_based = offset - 1u;
    if ((zero_based % guest_scope.stride) != 0)
        deny_guest_bridge();
    const uint32_t index = zero_based / guest_scope.stride;
    if (index >= guest_scope.next_guest_slot || !guest_scope.live[index])
        deny_guest_bridge();
    /* Guest offsets are monotonically assigned logical slots. The trusted
     * sidecar maps them to generation-checked physical allocator refs; the
     * physical slot may be reused without reissuing an old guest offset. */
    return index;
}

void ores_actor_region_guest_v1_begin(uint32_t object_slots,
                                      uint32_t slot_bytes,
                                      uint32_t scratch_bytes) {
    if (guest_scope.region || !object_slots ||
        object_slots > ORES_GUEST_REGION_MAX_SLOTS ||
        !slot_bytes || slot_bytes > ORES_GUEST_REGION_MAX_SLOT_BYTES ||
        !scratch_bytes || scratch_bytes > ORES_GUEST_REGION_MAX_SCRATCH_BYTES)
        deny_guest_bridge();

    const uint64_t identity = atomic_fetch_add_explicit(
        &next_guest_identity, 1, memory_order_relaxed);
    if (!identity || identity == UINT64_MAX) deny_guest_bridge();

    OresActorRegionV1 *region = ores_actor_region_v1_open(
        identity, identity, object_slots, slot_bytes, scratch_bytes);
    if (!ores_actor_region_v1_prefault(region)) deny_guest_bridge();
    const OresArenaTurn turn = ores_actor_region_v1_enter(
        region, identity, identity);

    guest_scope.region = region;
    guest_scope.turn = turn;
    guest_scope.slots = object_slots;
    guest_scope.stride = slot_bytes;
    guest_scope.live_count = 0;
    guest_scope.next_guest_slot = 0;
}

uint32_t ores_actor_region_guest_v1_alloc(uint32_t object_bytes) {
    require_scope();
    if (!object_bytes || object_bytes > guest_scope.stride ||
        guest_scope.live_count >= guest_scope.slots ||
        guest_scope.next_guest_slot >= guest_scope.slots)
        deny_guest_bridge();
    const OresActorRegionV1Ref ref = ores_actor_region_v1_alloc(
        guest_scope.region, guest_scope.turn, object_bytes);
    const uint32_t index = guest_scope.next_guest_slot++;
    const uint64_t logical_offset =
        (uint64_t)index * guest_scope.stride + UINT64_C(1);
    if (logical_offset > UINT32_MAX || guest_scope.live[index])
        deny_guest_bridge();
    guest_scope.refs[index] = ref;
    guest_scope.live[index] = 1;
    ++guest_scope.live_count;
    return (uint32_t)logical_offset;
}

int64_t ores_actor_region_guest_v1_read_i64(uint32_t offset,
                                            uint32_t field_offset) {
    const uint32_t index = checked_index(offset);
    int64_t value = 0;
    ores_actor_region_v1_read(guest_scope.region, guest_scope.turn,
        guest_scope.refs[index], field_offset, &value, (uint32_t)sizeof(value));
    return value;
}

void ores_actor_region_guest_v1_write_i64(uint32_t offset,
                                          uint32_t field_offset,
                                          int64_t value) {
    const uint32_t index = checked_index(offset);
    ores_actor_region_v1_write(guest_scope.region, guest_scope.turn,
        guest_scope.refs[index], field_offset, &value, (uint32_t)sizeof(value));
}

void ores_actor_region_guest_v1_drop(uint32_t offset) {
    const uint32_t index = checked_index(offset);
    ores_actor_region_v1_drop(guest_scope.region, guest_scope.turn,
        &guest_scope.refs[index]);
    guest_scope.live[index] = 0;
    if (!guest_scope.live_count) deny_guest_bridge();
    --guest_scope.live_count;
}

void ores_actor_region_guest_v1_end(void) {
    require_scope();
    /* A leak is a compiler/runtime contract failure, never silently swept up
     * by the region close path. Every admitted object must have a real drop. */
    if (guest_scope.live_count != 0) deny_guest_bridge();
    OresActorRegionV1 *region = guest_scope.region;
    const OresArenaTurn turn = guest_scope.turn;
    ores_actor_region_v1_leave(region, turn);
    ores_actor_region_v1_retire(region);
    if (!ores_actor_region_v1_try_destroy(region)) deny_guest_bridge();

    /* Scrub the trusted sidecar after the backing allocator has been retired. */
    volatile unsigned char *p = (volatile unsigned char *)&guest_scope;
    for (size_t i = 0; i < sizeof(guest_scope); ++i) p[i] = 0;
}
