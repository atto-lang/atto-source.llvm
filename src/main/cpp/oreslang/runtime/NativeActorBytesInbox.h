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
/* Irreversible pre-admission mode transition, only for a pristine inbox
 * matching the exact actor incarnation. Once bound, legacy unsequenced
 * send/receive fail closed; all user DATA must use a scheduler ordinal.
 * Binding itself is not authorization to invent scheduler mail sequences. */
int ores_native_actor_bytes_bind_sequenced(OresNativeActorBytesInbox* inbox,
    OresNativeActorBytesRef owner);

/* Admission means copied into the TARGET actor's preallocated storage,
 * never that guest code consumed the message. No actor carrier waits. */
int ores_native_actor_bytes_try_send(OresNativeActorBytesInbox* inbox,
    OresNativeActorBytesRef target, const void* bytes, size_t length);
/* Sequenced admission for scheduler transaction ONLY. A positive,
 * scheduler-minted ordinal tags the exact DATA turn, never an untrusted guest
 * field. A repeated/out-of-order ordinal is refused without changing inbox
 * state. This function itself does not enqueue a scheduler DATA message;
 * invoke it only through the trusted scheduler commit callback. */
int ores_native_actor_bytes_try_send_sequenced(OresNativeActorBytesInbox* inbox,
    OresNativeActorBytesRef target, const void* bytes, size_t length,
    uint64_t source_mail_sequence);
/* One externally linked commit function is required for the scheduler's
 * exact callback-identity check. Header-local static inline function
 * pointers can differ across translation units and MUST NOT be used as
 * authorization identities. This is trusted host ABI, never guest FFI. */
typedef struct {
    OresNativeActorBytesInbox* inbox;
    OresNativeActorBytesRef storage;
    const void* bytes;
    size_t length;
} OresNativeActorBytesCommitContext;
int ores_native_actor_bytes_commit_payload(
    void* context, uint64_t admitted_sequence);
/* Only a trusted runtime actor turn may call this. A source-level receiver
 * capability/borrow checker is still required; the ref alone is not proof
 * of a physical actor turn. On SMALL, required_length is set and no read
 * or mutation occurs. Never returns pointers to inbox-owned memory. */
int ores_native_actor_bytes_try_receive(OresNativeActorBytesInbox* inbox,
    OresNativeActorBytesRef receiver, void* output, size_t capacity,
    size_t* required_length);
/* Only the authenticated physical DATA-mail with matching ordinal may
 * dequeue its sequenced payload. Mismatch, including legacy unsequenced
 * frames, does not dequeue or mutate output. This call is trusted-host-only;
 * the scheduler binding must certify physical current-turn authority. */
int ores_native_actor_bytes_try_receive_sequenced(OresNativeActorBytesInbox* inbox,
    OresNativeActorBytesRef receiver, uint64_t source_mail_sequence,
    void* output, size_t capacity, size_t* required_length);
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
