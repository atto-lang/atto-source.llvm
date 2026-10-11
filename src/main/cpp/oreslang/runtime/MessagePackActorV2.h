#ifndef ORESLANG_MESSAGEPACK_ACTOR_V2_H
#define ORESLANG_MESSAGEPACK_ACTOR_V2_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/*
 * Canonical transport is MessagePack v2, byte-identical to the Java
 * MessagePackCollectionCodec: [2, schema-id, bin8(sha256(shape)), payload].
 * DO NOT use the experimental OMSG/1 frame as the actor's default wire ABI.
 *
 * Contract data is trusted COMPILER output and must remain alive until the
 * scheduler has drained. Numeric/string identity is not authentication.
 * Shape grammar: B I F S T[...] FA[...] FL[...] A[shape] L[shape].
 */
typedef struct {
    const char* schema_id;
    const char* shape;
    const uint8_t* shape_sha256; /* exactly 32 bytes, compiler-pinned */
} OresMp2Contract;
typedef struct {
    uint8_t* bytes;
    size_t size;
} OresMp2Owned;
enum { ORES_MP2_MAX_BYTES=1048576, ORES_MP2_MAX_NODES=100000, ORES_MP2_MAX_DEPTH=64 };

int ores_mp2_validate_copy(const OresMp2Contract* contract,
                           const uint8_t* bytes, size_t size, OresMp2Owned* out);
void ores_mp2_owned_release(OresMp2Owned* value);

typedef struct OresMp2ActorGroup OresMp2ActorGroup;
typedef struct {
    uint64_t scheduler_domain;
    uint64_t actor_slot;
    uint64_t generation;
} OresMp2ActorRef;
/* Callback may read bytes only for this admitted actor turn. It must not
 * retain the pointer. The owner releases and zeroizes after it returns.
 */
typedef void (*OresMp2OnMessage)(void* context, size_t actor_slot,
                                  const uint8_t* bytes, size_t size);
OresMp2ActorGroup* ores_mp2_group_open(size_t actors, size_t queue_capacity,
                                      size_t carriers, OresMp2OnMessage on_message,
                                      void* context);
/* Set once per slot, before spawn. Contract memory must outlive the group. */
int ores_mp2_group_contract(OresMp2ActorGroup* group, size_t slot,
                            OresMp2Contract contract);
OresMp2ActorRef ores_mp2_group_spawn(OresMp2ActorGroup* group, size_t slot);
/* Encodes/copies BEFORE scheduler admission; destination contract is
 * validated. Caller always retains input memory. Reject frees the copy.
 * Successful admission moves the owned snapshot into exactly one lane turn.
 */
int ores_mp2_group_send(OresMp2ActorGroup* group, OresMp2ActorRef target,
                        const uint8_t* wire, size_t size);
int ores_mp2_group_stop(OresMp2ActorGroup* group, OresMp2ActorRef target);
int ores_mp2_group_wait_done(OresMp2ActorGroup* group, OresMp2ActorRef target);
/* Only a trusted, non-carrier caller after producer fencing may destroy.
 * Calls retire + drain and joins worker carriers. */
int ores_mp2_group_destroy(OresMp2ActorGroup* group);
#ifdef __cplusplus
}
#endif
#endif
