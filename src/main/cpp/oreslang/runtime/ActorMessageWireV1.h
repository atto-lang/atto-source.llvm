#ifndef ORESLANG_ACTOR_MESSAGE_WIRE_V1_H
#define ORESLANG_ACTOR_MESSAGE_WIRE_V1_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* ABI v1: OMSG | version=1 | flags=0 | reserved=0 | BE schema:u64 |
 * BE variant:u32 | BE payload_len:u32 | one recursively tagged value.
 * The native payload tree is copied into an owned buffer. An actor scheduler
 * may retain that buffer across carrier migration; it must release it once
 * the message has been consumed or abandoned.
 *
 * These are internal host/compiler APIs, not guest-visible operations.
 * The compiler must validate Serializable<T> and the exact receiving
 * message domain before calling pack. A matching id alone is NOT a type proof.
 */
#define ORES_ACTOR_WIRE_V1_HEADER_SIZE 24u
#define ORES_ACTOR_WIRE_V1_MAX_FRAME (1024u * 1024u)
#define ORES_ACTOR_WIRE_V1_MAX_NODES 4096u
#define ORES_ACTOR_WIRE_V1_MAX_DEPTH 64u

enum OresActorWireV1Tag {
    ORES_ACTOR_WIRE_BOOL = 1,
    ORES_ACTOR_WIRE_INT64 = 2,
    ORES_ACTOR_WIRE_STRING = 3,
    ORES_ACTOR_WIRE_TUPLE = 4,
    ORES_ACTOR_WIRE_ARRAY = 5,
    ORES_ACTOR_WIRE_LIST = 6
};

typedef struct OresActorWireV1Frame {
    uint8_t* bytes;
    size_t size;
} OresActorWireV1Frame;

/* Return 1 on success, 0 on invalid input, overflow or allocation failure.
 * Each function expects a fresh (zeroed) out frame. Never alias source.
 * A valid payload consists of exactly one canonically encoded tagged node.
 */
int ores_actor_wire_v1_pack(uint64_t schema_id, uint32_t variant_id,
                            const uint8_t* payload, size_t payload_size,
                            OresActorWireV1Frame* out);
int ores_actor_wire_v1_unpack(const uint8_t* frame, size_t frame_size,
                              uint64_t expected_schema, uint32_t expected_variant,
                              OresActorWireV1Frame* out);
void ores_actor_wire_v1_release(OresActorWireV1Frame* owned);
#ifdef __cplusplus
}
#endif
#endif
