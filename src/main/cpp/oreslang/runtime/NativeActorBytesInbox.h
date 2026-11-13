#ifndef ORESLANG_NATIVE_ACTOR_BYTES_INBOX_H
#define ORESLANG_NATIVE_ACTOR_BYTES_INBOX_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Trusted native scheduler substrate, NOT a guest-callable API. An immutable
 * reference names exactly one actor incarnation, even if its numeric actor ID
 * is later reused. No raw bytes pointer or storage address is returned. */
typedef struct OresNativeActorBytesInbox OresNativeActorBytesInbox;
typedef struct {
    uint64_t actor_id;
    uint64_t incarnation;
    uint64_t nonce;
} OresNativeActorBytesRef;

enum OresNativeActorBytesStatus {
    ORES_ACTOR_BYTES_DENIED = 0,   /* full, empty, stale or retired */
    ORES_ACTOR_BYTES_OK = 1,
    ORES_ACTOR_BYTES_BUSY = -1,    /* concurrent operation: yield/retry later */
    ORES_ACTOR_BYTES_SMALL = -2    /* receiver buffer insufficient; no dequeue */
};

/* Fixed actor-owned storage, allocated once on admission. No malloc/free in
 * try_send or try_receive. Limits: 1..128 slots, 1..65536 per message, and
 * at most 8 MiB preallocated. byte_budget <= physical slab capacity.
 * An inbox belongs to ONE actor; no shared mutable slab across actors.
 * The caller must bind its lifecycle to a genuine generation-fenced OresVM
 * actor and prove all external producers quiescent before physical destroy. */
OresNativeActorBytesInbox* ores_native_actor_bytes_open(
    uint64_t actor_id, uint64_t incarnation, size_t slots,
    size_t max_message_bytes, size_t byte_budget);
OresNativeActorBytesRef ores_native_actor_bytes_ref(
    const OresNativeActorBytesInbox* inbox);

/* Admission means copied into the TARGET actor's preallocated storage,
 * never that guest code consumed the message. No actor carrier waits. */
int ores_native_actor_bytes_try_send(OresNativeActorBytesInbox* inbox,
    OresNativeActorBytesRef target, const void* bytes, size_t length);
/* Only a trusted runtime actor turn may call this. A source-level receiver
 * capability/borrow checker is still required; the ref alone is not proof
 * of a physical actor turn. On SMALL, required_length is set and no read
 * or mutation occurs. Never returns pointers to inbox-owned memory. */
int ores_native_actor_bytes_try_receive(OresNativeActorBytesInbox* inbox,
    OresNativeActorBytesRef receiver, void* output, size_t capacity,
    size_t* required_length);
/* Atomically closes admission, abandons pending messages and zeroizes every
 * slot. Caller must retry BUSY from CONTROL, not spin/park an actor thread. */
int ores_native_actor_bytes_try_retire(OresNativeActorBytesInbox* inbox,
    OresNativeActorBytesRef owner);
/* Only AFTER scheduler STOP/DONE and all producers/receivers are physically
 * quiescent. API cannot independently establish external pointer quiescence. */
int ores_native_actor_bytes_try_destroy(OresNativeActorBytesInbox* inbox,
    OresNativeActorBytesRef owner);
int ores_native_actor_bytes_observe(OresNativeActorBytesInbox* inbox,
    OresNativeActorBytesRef owner, size_t* pending, size_t* used_bytes);
#ifdef ORESLANG_ACTOR_BYTES_TESTING
/* Test-only verification: no raw data leaves this function. */
int ores_native_actor_bytes_all_zero_for_test(OresNativeActorBytesInbox* inbox);
#endif
#ifdef __cplusplus
}
#endif
#endif
