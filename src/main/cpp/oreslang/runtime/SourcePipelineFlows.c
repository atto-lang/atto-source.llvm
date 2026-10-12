#include "oreslang/runtime/SourcePipelineFlows.h"
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* Intentionally independent of the legacy single-flow global phase.
 * The runtime adapter must preserve scheduler producer leases, actor turn
 * finalization, and generation-checked mailbox publication. No guest FFI. */
typedef struct {
    uint64_t id;
    uint64_t pipeline_id;
    uint64_t route_generation;
    uint64_t last_message;
    uint8_t stage_count;
    OresFlowActor stages[ORES_FLOW_MAX_STAGES];
    uint32_t phase;
    OresFlowTerminal state;
    int reserved;
    OresFlowTicket ticket;
} Flow;

struct OresFlowTable {
    pthread_mutex_t mutex;
    uint64_t next_flow;
    uint64_t next_message;
    Flow flows[ORES_FLOW_MAX_ACTIVE];
};

static int actor_equal(OresFlowActor a, OresFlowActor b) {
    return a.actor_id == b.actor_id &&
           a.actor_generation == b.actor_generation;
}
static int valid_actor(OresFlowActor actor) {
    return actor.actor_id != 0 && actor.actor_generation != 0;
}
static Flow* find_flow(OresFlowTable* table, uint64_t id) {
    for (size_t i = 0; i < ORES_FLOW_MAX_ACTIVE; ++i)
        if (table->flows[i].id == id) return &table->flows[i];
    return NULL;
}
static int ticket_equal(const OresFlowTicket* a, const OresFlowTicket* b) {
    return a->flow_id == b->flow_id &&
           a->correlation_id == b->correlation_id &&
           a->pipeline_id == b->pipeline_id &&
           a->route_generation == b->route_generation &&
           a->message_id == b->message_id &&
           a->parent_message_id == b->parent_message_id &&
           actor_equal(a->sender, b->sender) &&
           actor_equal(a->destination, b->destination) &&
           a->direction == b->direction && a->stage_index == b->stage_index;
}
OresFlowTable* ores_flow_table_create(void) {
    OresFlowTable* table = calloc(1, sizeof(*table));
    if (!table) return NULL;
    if (pthread_mutex_init(&table->mutex, NULL) != 0) {
        free(table);
        return NULL;
    }
    table->next_flow = 1;
    table->next_message = 1;
    return table;
}
void ores_flow_table_destroy(OresFlowTable* table) {
    if (!table) return;
    /* The trusted scheduler owner must fence/join all callers first.
     * Never silently destroy a mutex while another producer owns it. */
    if (pthread_mutex_destroy(&table->mutex) != 0) abort();
    free(table);
}
OresFlowError ores_flow_open(OresFlowTable* table, uint64_t pipeline_id,
    uint64_t route_generation, const OresFlowActor* stages,
    uint8_t stage_count, uint64_t* flow_id) {
    if (!table || !pipeline_id || !route_generation || !stages || !flow_id ||
        stage_count < 2 || stage_count > ORES_FLOW_MAX_STAGES)
        return ORES_FLOW_BAD_ARGUMENT;
    for (uint8_t i = 0; i < stage_count; ++i) {
        if (!valid_actor(stages[i])) return ORES_FLOW_BAD_ARGUMENT;
        for (uint8_t j = 0; j < i; ++j)
            if (actor_equal(stages[i], stages[j]))
                return ORES_FLOW_BAD_ARGUMENT;
    }
    pthread_mutex_lock(&table->mutex);
    if (table->next_flow == UINT64_MAX) {
        pthread_mutex_unlock(&table->mutex);
        return ORES_FLOW_CAPACITY; /* never wrap/reuse flow identity */
    }
    Flow* slot = NULL;
    for (size_t i = 0; i < ORES_FLOW_MAX_ACTIVE; ++i) {
        if (!table->flows[i].id) {
            slot = &table->flows[i];
            break;
        }
    }
    if (!slot) {
        pthread_mutex_unlock(&table->mutex);
        return ORES_FLOW_CAPACITY;
    }
    memset(slot, 0, sizeof(*slot));
    slot->id = table->next_flow++;
    slot->pipeline_id = pipeline_id;
    slot->route_generation = route_generation;
    slot->stage_count = stage_count;
    memcpy(slot->stages, stages, stage_count * sizeof(stages[0]));
    slot->state = ORES_FLOW_ACTIVE;
    *flow_id = slot->id;
    pthread_mutex_unlock(&table->mutex);
    return ORES_FLOW_OK;
}
OresFlowError ores_flow_reserve(OresFlowTable* table, uint64_t flow_id,
    OresFlowActor sender, OresFlowDirection direction, uint64_t causal_parent,
    OresFlowTicket* out) {
    if (!table || !flow_id || !out) return ORES_FLOW_BAD_ARGUMENT;
    pthread_mutex_lock(&table->mutex);
    Flow* f = find_flow(table, flow_id);
    OresFlowError error = ORES_FLOW_OK;
    if (!f) error = ORES_FLOW_NOT_FOUND;
    else if (f->state != ORES_FLOW_ACTIVE) error = ORES_FLOW_TERMINAL;
    else if (f->reserved) error = ORES_FLOW_ALREADY_RESERVED;
    else if (f->phase >= 2u * f->stage_count - 1u)
        error = ORES_FLOW_INCOMPLETE; /* all DATA hops already committed */
    else if (causal_parent != f->last_message)
        error = ORES_FLOW_BAD_CAUSAL_PARENT;
    else {
        uint32_t phase = f->phase;
        uint32_t n = f->stage_count;
        OresFlowActor expected_sender = {0, 0};
        OresFlowDirection expected_direction;
        uint8_t destination;
        if (phase == 0) {
            expected_direction = ORES_FLOW_ROOT;
            destination = 0;
        } else if (phase < n) {
            expected_direction = ORES_FLOW_FORWARD;
            expected_sender = f->stages[phase - 1];
            destination = (uint8_t)phase;
        } else {
            expected_direction = ORES_FLOW_BACKWARD;
            expected_sender = f->stages[2u * (n - 1u) - phase + 1u];
            destination = (uint8_t)(2u * (n - 1u) - phase);
        }
        if (!actor_equal(sender, expected_sender) ||
            direction != expected_direction)
            error = ORES_FLOW_WRONG_SENDER;
        else if (table->next_message == UINT64_MAX)
            error = ORES_FLOW_CAPACITY; /* no message-ID wrap */
        else {
            OresFlowTicket t = {0};
            t.flow_id = f->id;
            t.correlation_id = f->id;
            t.pipeline_id = f->pipeline_id;
            t.route_generation = f->route_generation;
            t.message_id = table->next_message++;
            t.parent_message_id = f->last_message;
            t.sender = sender;
            t.destination = f->stages[destination];
            t.stage_index = destination;
            t.direction = (uint8_t)direction;
            f->ticket = t;
            f->reserved = 1;
            *out = t;
        }
    }
    pthread_mutex_unlock(&table->mutex);
    return error;
}
OresFlowError ores_flow_publish(OresFlowTable* table,
    const OresFlowTicket* ticket, OresFlowTryPublish try_publish, void* ctx) {
    if (!table || !ticket || !try_publish) return ORES_FLOW_BAD_ARGUMENT;
    pthread_mutex_lock(&table->mutex);
    Flow* f = find_flow(table, ticket->flow_id);
    OresFlowError error = ORES_FLOW_OK;
    if (!f) error = ORES_FLOW_NOT_FOUND;
    else if (f->state != ORES_FLOW_ACTIVE) error = ORES_FLOW_TERMINAL;
    else if (!f->reserved || !ticket_equal(ticket, &f->ticket))
        error = ORES_FLOW_BAD_TICKET;
    else {
        /* Linearization point: terminal cancel and nonblocking scheduler
         * publication share this mutex. try_publish MUST NOT wait. */
        if (try_publish(ctx, ticket) == 1) {
            f->last_message = ticket->message_id;
            ++f->phase;
        } else {
            f->state = ORES_FLOW_REJECTED;
            error = ORES_FLOW_REJECTED_MAIL;
        }
        f->reserved = 0;
        memset(&f->ticket, 0, sizeof(f->ticket));
    }
    pthread_mutex_unlock(&table->mutex);
    return error;
}
OresFlowError ores_flow_cancel(OresFlowTable* table, uint64_t flow_id,
    OresFlowActor source) {
    if (!table || !flow_id) return ORES_FLOW_BAD_ARGUMENT;
    pthread_mutex_lock(&table->mutex);
    Flow* f = find_flow(table, flow_id);
    OresFlowError error;
    if (!f) error = ORES_FLOW_NOT_FOUND;
    else if (!actor_equal(source, f->stages[0]))
        error = ORES_FLOW_WRONG_SENDER;
    else if (f->state != ORES_FLOW_ACTIVE) error = ORES_FLOW_TERMINAL;
    else {
        /* Reserved-but-not-published DATA is invalidated, never admitted. */
        f->state = ORES_FLOW_CANCELLED;
        f->reserved = 0;
        memset(&f->ticket, 0, sizeof(f->ticket));
        error = ORES_FLOW_OK;
    }
    pthread_mutex_unlock(&table->mutex);
    return error;
}
OresFlowError ores_flow_finish(OresFlowTable* table, uint64_t flow_id,
    OresFlowActor source, uint64_t causal_parent) {
    if (!table || !flow_id) return ORES_FLOW_BAD_ARGUMENT;
    pthread_mutex_lock(&table->mutex);
    Flow* f = find_flow(table, flow_id);
    OresFlowError error;
    if (!f) error = ORES_FLOW_NOT_FOUND;
    else if (!actor_equal(source, f->stages[0])) error = ORES_FLOW_WRONG_SENDER;
    else if (f->state != ORES_FLOW_ACTIVE) error = ORES_FLOW_TERMINAL;
    else if (f->reserved || f->phase != 2u * f->stage_count - 1u)
        error = ORES_FLOW_INCOMPLETE;
    else if (causal_parent != f->last_message)
        error = ORES_FLOW_BAD_CAUSAL_PARENT;
    else {
        /* Caller must verify the source DATA turn/finalizer completed. */
        f->state = ORES_FLOW_COMPLETED;
        error = ORES_FLOW_OK;
    }
    pthread_mutex_unlock(&table->mutex);
    return error;
}
OresFlowError ores_flow_snapshot(OresFlowTable* table, uint64_t flow_id,
    OresFlowSnapshot* out) {
    if (!table || !out || !flow_id) return ORES_FLOW_BAD_ARGUMENT;
    pthread_mutex_lock(&table->mutex);
    Flow* f = find_flow(table, flow_id);
    if (!f) {
        pthread_mutex_unlock(&table->mutex);
        return ORES_FLOW_NOT_FOUND;
    }
    out->state = f->state;
    out->last_message_id = f->last_message;
    out->correlation_id = f->id;
    out->committed_hops = f->phase;
    out->expected_hops = 2u * f->stage_count - 1u;
    out->reserved = (uint8_t)f->reserved;
    pthread_mutex_unlock(&table->mutex);
    return ORES_FLOW_OK;
}
OresFlowError ores_flow_retire(OresFlowTable* table, uint64_t flow_id) {
    if (!table || !flow_id) return ORES_FLOW_BAD_ARGUMENT;
    pthread_mutex_lock(&table->mutex);
    Flow* f = find_flow(table, flow_id);
    OresFlowError error;
    if (!f) error = ORES_FLOW_NOT_FOUND;
    else if (f->state == ORES_FLOW_ACTIVE || f->reserved)
        error = ORES_FLOW_INCOMPLETE;
    else {
        memset(f, 0, sizeof(*f));
        error = ORES_FLOW_OK;
    }
    pthread_mutex_unlock(&table->mutex);
    return error;
}
