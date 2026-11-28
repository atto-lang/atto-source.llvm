#ifndef ATTO_PROXY_RECEIPT_GATE_V1_H
#define ATTO_PROXY_RECEIPT_GATE_V1_H
/* Trusted-host receipt state machine, NOT a production wire/crypto ABI or
   compiled .atto actor. Caller must authenticate and authorize each event
   before atto_proxy_receipt_apply_verified. */
#include <stdbool.h>
#include <stdint.h>
typedef struct {
    uint32_t protocol_major;
    uint64_t tenant, caller_incarnation, target_actor, target_incarnation;
    uint64_t association_generation;
    uint64_t placement_epoch, message_id, attempt;
} atto_receipt_identity_v1;
typedef enum {
    ATTO_RECEIPT_ADMITTED, ATTO_RECEIPT_STARTED, ATTO_RECEIPT_PROCESSED,
    ATTO_RECEIPT_DURABLE_INBOX, ATTO_RECEIPT_DURABLE_RESULT,
    ATTO_RECEIPT_REJECTED, ATTO_RECEIPT_FAILED, ATTO_RECEIPT_INDETERMINATE
} atto_receipt_event_v1;
typedef enum { ATTO_STAGE_DISPATCHED, ATTO_STAGE_ADMITTED,
               ATTO_STAGE_STARTED, ATTO_STAGE_PROCESSED } atto_receipt_stage_v1;
typedef enum { ATTO_OUTCOME_PENDING, ATTO_OUTCOME_REJECTED,
               ATTO_OUTCOME_FAILED, ATTO_OUTCOME_INDETERMINATE } atto_receipt_outcome_v1;
typedef struct {
    atto_receipt_identity_v1 expected;
    atto_receipt_stage_v1 stage;
    atto_receipt_outcome_v1 outcome;
    bool durable_inbox, durable_result;
} atto_proxy_receipt_gate_v1;
void atto_proxy_receipt_init(atto_proxy_receipt_gate_v1 *gate,
                             atto_receipt_identity_v1 expected);
bool atto_proxy_receipt_apply_verified(atto_proxy_receipt_gate_v1 *gate,
                                        atto_receipt_identity_v1 observed,
                                        atto_receipt_event_v1 event);
#endif
