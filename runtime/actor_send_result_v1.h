#ifndef ATTO_ACTOR_SEND_RESULT_V1_H
#define ATTO_ACTOR_SEND_RESULT_V1_H

#include <stdint.h>

/* Native host-only, fixed-width admission result. The tag describes mailbox
 * admission, not message processing or durable completion. */
typedef struct AttoActorSendResultV1 {
    uint32_t tag;    /* 0 = Ok(Ack), 1 = Err(SendError) */
    uint32_t reason; /* 0 on Ok, 1 unavailable, 2 full */
} AttoActorSendResultV1;

enum {
    ATTO_SEND_ADMITTED_V1 = 0u,
    ATTO_SEND_REJECTED_V1 = 1u,
    ATTO_SEND_ERROR_NONE_V1 = 0u,
    ATTO_SEND_ERROR_UNAVAILABLE_V1 = 1u,
    ATTO_SEND_ERROR_FULL_V1 = 2u
};

_Static_assert(sizeof(AttoActorSendResultV1) == 8u,
               "actor send v1 ABI must remain two u32 fields");
_Static_assert(_Alignof(AttoActorSendResultV1) == _Alignof(uint32_t),
               "actor send v1 ABI must have u32 alignment");

/* Only the trusted native runtime may implement this entrypoint. Guest code
 * cannot forge an admission result to modify the authoritative mailbox. */
AttoActorSendResultV1 atto_actor_send_result_v1(int32_t actor_ref, int64_t value);

#endif
