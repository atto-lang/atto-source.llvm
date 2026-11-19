#include "atto/proxy_receipt_gate_v1.h"
/* No actor carrier, socket, credential, or guest callback is accessed here.
   The verified suffix is a caller obligation, not proof of authentication. */
static bool valid(const atto_receipt_identity_v1 *i) {
    return i->protocol_major == 1 && i->tenant != 0 &&
        i->caller_incarnation != 0 && i->target_actor != 0 &&
        i->target_incarnation != 0 && i->association_generation != 0 &&
        i->placement_epoch != 0 &&
        i->message_id != 0 && i->attempt != 0;
}
static bool match(const atto_receipt_identity_v1 *a,
                  const atto_receipt_identity_v1 *b) {
    return a->protocol_major == b->protocol_major &&
        a->tenant == b->tenant && a->caller_incarnation == b->caller_incarnation &&
        a->target_actor == b->target_actor &&
        a->target_incarnation == b->target_incarnation &&
        a->association_generation == b->association_generation &&
        a->placement_epoch == b->placement_epoch &&
        a->message_id == b->message_id && a->attempt == b->attempt;
}
void atto_proxy_receipt_init(atto_proxy_receipt_gate_v1 *gate,
                             atto_receipt_identity_v1 expected) {
    if (!gate) return;
    *gate = (atto_proxy_receipt_gate_v1){0};
    gate->expected = expected;
}
bool atto_proxy_receipt_apply_verified(atto_proxy_receipt_gate_v1 *gate,
                                        atto_receipt_identity_v1 observed,
                                        atto_receipt_event_v1 event) {
    if (!gate || !valid(&gate->expected) || !match(&gate->expected, &observed) ||
        gate->outcome != ATTO_OUTCOME_PENDING)
        return false;
    switch (event) {
    case ATTO_RECEIPT_ADMITTED:
        if (gate->stage < ATTO_STAGE_ADMITTED) gate->stage = ATTO_STAGE_ADMITTED;
        return true;
    case ATTO_RECEIPT_STARTED:
        if (gate->stage < ATTO_STAGE_ADMITTED) return false;
        if (gate->stage < ATTO_STAGE_STARTED) gate->stage = ATTO_STAGE_STARTED;
        return true;
    case ATTO_RECEIPT_PROCESSED:
        if (gate->stage < ATTO_STAGE_STARTED) return false;
        gate->stage = ATTO_STAGE_PROCESSED;
        return true;
    case ATTO_RECEIPT_DURABLE_INBOX:
        if (gate->stage < ATTO_STAGE_ADMITTED) return false;
        gate->durable_inbox = true;
        return true;
    case ATTO_RECEIPT_DURABLE_RESULT:
        if (gate->stage != ATTO_STAGE_PROCESSED) return false;
        gate->durable_result = true;
        return true;
    case ATTO_RECEIPT_REJECTED:
        if (gate->stage != ATTO_STAGE_DISPATCHED) return false;
        gate->outcome = ATTO_OUTCOME_REJECTED;
        return true;
    case ATTO_RECEIPT_FAILED:
        if (gate->stage < ATTO_STAGE_ADMITTED || gate->stage == ATTO_STAGE_PROCESSED)
            return false;
        gate->outcome = ATTO_OUTCOME_FAILED;
        return true;
    case ATTO_RECEIPT_INDETERMINATE:
        if (gate->stage == ATTO_STAGE_PROCESSED) return false;
        gate->outcome = ATTO_OUTCOME_INDETERMINATE;
        return true;
    default: return false;
    }
}
