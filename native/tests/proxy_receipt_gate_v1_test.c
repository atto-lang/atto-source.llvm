#include "atto/proxy_receipt_gate_v1.h"
#include <assert.h>
int main(void) {
    atto_receipt_identity_v1 id = {1, 11, 12, 13, 14, 17, 15, 16, 1};
    atto_proxy_receipt_gate_v1 gate;
    atto_proxy_receipt_init(&gate, id);
    assert(gate.stage == ATTO_STAGE_DISPATCHED); /* local enqueue != admitted */
    assert(!atto_proxy_receipt_apply_verified(&gate, id, ATTO_RECEIPT_STARTED));
    assert(!atto_proxy_receipt_apply_verified(&gate, id, ATTO_RECEIPT_PROCESSED));
    assert(!atto_proxy_receipt_apply_verified(&gate, id, ATTO_RECEIPT_DURABLE_RESULT));
    atto_receipt_identity_v1 wrong = id;
    wrong.tenant++;
    assert(!atto_proxy_receipt_apply_verified(&gate, wrong, ATTO_RECEIPT_ADMITTED));
    wrong = id; wrong.caller_incarnation++;
    assert(!atto_proxy_receipt_apply_verified(&gate, wrong, ATTO_RECEIPT_ADMITTED));
    wrong = id; wrong.target_incarnation++;
    assert(!atto_proxy_receipt_apply_verified(&gate, wrong, ATTO_RECEIPT_ADMITTED));
    wrong = id; wrong.association_generation++;
    assert(!atto_proxy_receipt_apply_verified(&gate, wrong, ATTO_RECEIPT_ADMITTED));
    wrong = id; wrong.placement_epoch++;
    assert(!atto_proxy_receipt_apply_verified(&gate, wrong, ATTO_RECEIPT_ADMITTED));
    wrong = id; wrong.attempt++;
    assert(!atto_proxy_receipt_apply_verified(&gate, wrong, ATTO_RECEIPT_ADMITTED));
    wrong = id; wrong.protocol_major++;
    assert(!atto_proxy_receipt_apply_verified(&gate, wrong, ATTO_RECEIPT_ADMITTED));
    assert(atto_proxy_receipt_apply_verified(&gate, id, ATTO_RECEIPT_ADMITTED));
    assert(!atto_proxy_receipt_apply_verified(&gate, id, ATTO_RECEIPT_DURABLE_RESULT));
    assert(atto_proxy_receipt_apply_verified(&gate, id, ATTO_RECEIPT_DURABLE_INBOX));
    assert(gate.durable_inbox && gate.stage == ATTO_STAGE_ADMITTED);
    assert(atto_proxy_receipt_apply_verified(&gate, id, ATTO_RECEIPT_STARTED));
    assert(atto_proxy_receipt_apply_verified(&gate, id, ATTO_RECEIPT_ADMITTED));
    assert(gate.stage == ATTO_STAGE_STARTED);
    assert(atto_proxy_receipt_apply_verified(&gate, id, ATTO_RECEIPT_PROCESSED));
    assert(atto_proxy_receipt_apply_verified(&gate, id, ATTO_RECEIPT_DURABLE_RESULT));
    assert(gate.durable_result);
    assert(!atto_proxy_receipt_apply_verified(&gate, id, ATTO_RECEIPT_FAILED));
    atto_proxy_receipt_init(&gate, id);
    assert(atto_proxy_receipt_apply_verified(&gate, id, ATTO_RECEIPT_INDETERMINATE));
    assert(!atto_proxy_receipt_apply_verified(&gate, id, ATTO_RECEIPT_ADMITTED));
    atto_proxy_receipt_init(&gate, id);
    assert(atto_proxy_receipt_apply_verified(&gate, id, ATTO_RECEIPT_REJECTED));
    assert(!atto_proxy_receipt_apply_verified(&gate, id, ATTO_RECEIPT_ADMITTED));
    atto_proxy_receipt_init(&gate, id);
    assert(atto_proxy_receipt_apply_verified(&gate, id, ATTO_RECEIPT_ADMITTED));
    assert(atto_proxy_receipt_apply_verified(&gate, id, ATTO_RECEIPT_FAILED));
    assert(!atto_proxy_receipt_apply_verified(&gate, id, ATTO_RECEIPT_PROCESSED));
    id.tenant = 0;
    atto_proxy_receipt_init(&gate, id);
    assert(!atto_proxy_receipt_apply_verified(&gate, id, ATTO_RECEIPT_ADMITTED));
    return 0;
}
