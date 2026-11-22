#ifndef ATTO_HOST_ACTOR_TICKET_V1_H
#define ATTO_HOST_ACTOR_TICKET_V1_H
/* Experimental trusted-host JNI/UDS-bridge-facing native mailbox receipts.
 * NOT a guest ABI, authentication primitive, wire format or TenantVM security
 * boundary. The gateway must authenticate caller, tenant and scoped actor
 * capability before invoking these entry points. The route is correlation
 * metadata, never proof of authorization.
 */
#include <stdint.h>
typedef struct {
    uint32_t protocol_major;
    uint64_t tenant;
    uint64_t caller_incarnation;
    uint64_t target_actor;
    uint64_t target_incarnation;
    uint64_t association_generation;
    uint64_t placement_epoch;
    uint64_t message_id;
    uint64_t attempt;
} atto_host_receipt_route_v1;
typedef enum {
    ATTO_TICKET_ADMITTED = 1,
    ATTO_TICKET_STARTED = 2,
    ATTO_TICKET_PROCESSED = 3
} atto_host_ticket_state_v1;
/* Return: 0=admitted, -1=invalid/stale/closed, -2=capacity full.
 * The host must preserve (actor ref, full route, ticket ID) as one tuple.
 * In this native prototype PROCESSSED means the void handler returned;
 * there is no separate typed value, per-turn recoverable exception, durable
 * commit, or restart-proof receipt. Failing native handlers terminate process.
 */
int32_t atto_host_actor_send_ticket(int32_t ref, int64_t value,
                                    const atto_host_receipt_route_v1 *route,
                                    uint64_t *out_ticket);
int32_t atto_host_actor_ticket_snapshot(int32_t ref, uint64_t ticket,
                                         const atto_host_receipt_route_v1 *route,
                                         atto_host_ticket_state_v1 *out_state);
int32_t atto_host_actor_ticket_release(int32_t ref, uint64_t ticket,
                                        const atto_host_receipt_route_v1 *route);
#endif
