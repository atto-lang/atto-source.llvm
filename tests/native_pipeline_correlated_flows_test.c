#include "oreslang/runtime/SourcePipelineFlows.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

static const OresFlowActor stages[3] = {{100, 10}, {101, 10}, {102, 10}};
static _Atomic unsigned delivered = 0;
static int accept(void* unused, const OresFlowTicket* ticket) {
    (void)unused;
    assert(ticket->correlation_id == ticket->flow_id);
    assert(ticket->pipeline_id == 17 && ticket->route_generation == 3);
    assert(ticket->destination.actor_generation == 10);
    atomic_fetch_add(&delivered, 1);
    return 1;
}
static int reject(void* unused, const OresFlowTicket* ticket) {
    (void)unused;
    (void)ticket;
    return 0;
}
static OresFlowActor sender_for(unsigned phase, unsigned n,
                                const OresFlowActor* actors) {
    if (phase == 0) return (OresFlowActor){0, 0};
    if (phase < n) return actors[phase - 1];
    return actors[2 * (n - 1) - phase + 1];
}
static OresFlowDirection direction_for(unsigned phase, unsigned n) {
    if (!phase) return ORES_FLOW_ROOT;
    return phase < n ? ORES_FLOW_FORWARD : ORES_FLOW_BACKWARD;
}
static void exercise(OresFlowTable* table, uint64_t id,
                     const OresFlowActor* actors, unsigned n) {
    uint64_t parent = 0;
    for (unsigned phase = 0; phase < 2 * n - 1; ++phase) {
        OresFlowTicket t = {0};
        assert(ores_flow_reserve(table, id, sender_for(phase, n, actors),
                  direction_for(phase, n), parent, &t) == ORES_FLOW_OK);
        assert(t.parent_message_id == parent);
        assert(t.route_generation == 3);
        assert(t.message_id != 0);
        assert(ores_flow_publish(table, &t, accept, NULL) == ORES_FLOW_OK);
        assert(ores_flow_publish(table, &t, accept, NULL)
               == ORES_FLOW_BAD_TICKET);
        parent = t.message_id;
    }
    assert(ores_flow_finish(table, id, actors[0], parent) == ORES_FLOW_OK);
    assert(ores_flow_finish(table, id, actors[0], parent) == ORES_FLOW_TERMINAL);
    OresFlowSnapshot s;
    assert(ores_flow_snapshot(table, id, &s) == ORES_FLOW_OK);
    assert(s.state == ORES_FLOW_COMPLETED && !s.reserved);
    assert(s.last_message_id == parent && s.committed_hops == 2 * n - 1);
}
typedef struct {
    OresFlowTable* table;
    uint64_t ids[10];
} Worker;
static void* worker(void* v) {
    Worker* w = (Worker*)v;
    for (size_t i = 0; i < 10; ++i) exercise(w->table, w->ids[i], stages, 3);
    return NULL;
}
static void basic_and_adversarial(OresFlowTable* table) {
    uint64_t a, b;
    OresFlowTicket t = {0};
    assert(ores_flow_open(table, 17, 3, stages, 3, &a) == ORES_FLOW_OK);
    assert(ores_flow_open(table, 17, 3, stages, 3, &b) == ORES_FLOW_OK);
    assert(a != b);
    assert(ores_flow_reserve(table, a, stages[0], ORES_FLOW_ROOT, 0, &t)
           == ORES_FLOW_WRONG_SENDER);
    assert(ores_flow_reserve(table, a, (OresFlowActor){0,0},
             ORES_FLOW_FORWARD, 0, &t) == ORES_FLOW_WRONG_SENDER);
    assert(ores_flow_reserve(table, a, (OresFlowActor){0,0},
             ORES_FLOW_ROOT, 100, &t) == ORES_FLOW_BAD_CAUSAL_PARENT);
    assert(ores_flow_reserve(table, a, (OresFlowActor){0,0},
             ORES_FLOW_ROOT, 0, &t) == ORES_FLOW_OK);
    assert(ores_flow_reserve(table, a, (OresFlowActor){0,0},
             ORES_FLOW_ROOT, 0, &t) == ORES_FLOW_ALREADY_RESERVED);
    OresFlowTicket forged = t;
    forged.route_generation++;
    assert(ores_flow_publish(table, &forged, accept, NULL)
           == ORES_FLOW_BAD_TICKET);
    forged = t;
    forged.destination.actor_generation++;
    assert(ores_flow_publish(table, &forged, accept, NULL)
           == ORES_FLOW_BAD_TICKET);
    assert(ores_flow_cancel(table, a, (OresFlowActor){100, 11})
           == ORES_FLOW_WRONG_SENDER);
    /* Producer paused after reservation, cancellation wins before publish. */
    unsigned before = atomic_load(&delivered);
    assert(ores_flow_cancel(table, a, stages[0]) == ORES_FLOW_OK);
    assert(ores_flow_publish(table, &t, accept, NULL) == ORES_FLOW_TERMINAL);
    assert(atomic_load(&delivered) == before);
    assert(ores_flow_reserve(table, a, (OresFlowActor){0,0},
             ORES_FLOW_ROOT, 0, &t) == ORES_FLOW_TERMINAL);
    assert(ores_flow_retire(table, a) == ORES_FLOW_OK);
    assert(ores_flow_publish(table, &t, accept, NULL) == ORES_FLOW_NOT_FOUND);
    exercise(table, b, stages, 3);
    assert(ores_flow_retire(table, b) == ORES_FLOW_OK);
    uint64_t reused;
    assert(ores_flow_open(table, 17, 3, stages, 3, &reused) == ORES_FLOW_OK);
    assert(reused != a && reused != b);
    assert(ores_flow_publish(table, &forged, accept, NULL)
           == ORES_FLOW_NOT_FOUND);
    assert(ores_flow_cancel(table, reused, stages[0]) == ORES_FLOW_OK);
    assert(ores_flow_retire(table, reused) == ORES_FLOW_OK);
}
static void rejection_and_incomplete(OresFlowTable* table) {
    uint64_t id;
    OresFlowTicket ticket = {0};
    assert(ores_flow_open(table, 17, 3, stages, 3, &id) == ORES_FLOW_OK);
    assert(ores_flow_finish(table, id, stages[0], 0) == ORES_FLOW_INCOMPLETE);
    assert(ores_flow_reserve(table, id, (OresFlowActor){0,0},
             ORES_FLOW_ROOT, 0, &ticket) == ORES_FLOW_OK);
    assert(ores_flow_publish(table, &ticket, reject, NULL)
           == ORES_FLOW_REJECTED_MAIL);
    OresFlowSnapshot s;
    assert(ores_flow_snapshot(table, id, &s) == ORES_FLOW_OK);
    assert(s.state == ORES_FLOW_REJECTED && s.committed_hops == 0 && !s.reserved);
    assert(ores_flow_finish(table, id, stages[0], 0) == ORES_FLOW_TERMINAL);
    assert(ores_flow_retire(table, id) == ORES_FLOW_OK);
}
static void capacity(OresFlowTable* table) {
    uint64_t ids[ORES_FLOW_MAX_ACTIVE];
    for (size_t i = 0; i < ORES_FLOW_MAX_ACTIVE; ++i)
        assert(ores_flow_open(table, 17, 3, stages, 3, &ids[i]) == ORES_FLOW_OK);
    uint64_t extra;
    assert(ores_flow_open(table, 17, 3, stages, 3, &extra) == ORES_FLOW_CAPACITY);
    for (size_t i = 0; i < ORES_FLOW_MAX_ACTIVE; ++i) {
        assert(ores_flow_cancel(table, ids[i], stages[0]) == ORES_FLOW_OK);
        assert(ores_flow_retire(table, ids[i]) == ORES_FLOW_OK);
    }
}
static int accept_route(void* ctx, const OresFlowTicket* ticket) {
    const OresFlowActor* actors = (const OresFlowActor*)ctx;
    assert(ticket->stage_index < 16);
    assert(ticket->destination.actor_id == actors[ticket->stage_index].actor_id);
    assert(ticket->destination.actor_generation ==
           actors[ticket->stage_index].actor_generation);
    return 1;
}
static void sixteen_stages(OresFlowTable* table) {
    OresFlowActor s[16];
    for (unsigned i = 0; i < 16; ++i)
        s[i] = (OresFlowActor){500 + i, 50 + i};
    uint64_t id, parent = 0;
    assert(ores_flow_open(table, 17, 3, s, 16, &id) == ORES_FLOW_OK);
    for (unsigned phase = 0; phase < 31; ++phase) {
        OresFlowTicket t;
        assert(ores_flow_reserve(table, id, sender_for(phase,16,s),
            direction_for(phase,16),parent,&t)==ORES_FLOW_OK);
        assert(ores_flow_publish(table,&t,accept_route,s)==ORES_FLOW_OK);
        parent=t.message_id;
    }
    assert(ores_flow_finish(table,id,s[0],parent)==ORES_FLOW_OK);
    OresFlowSnapshot snapshot;
    assert(ores_flow_snapshot(table,id,&snapshot)==ORES_FLOW_OK);
    assert(snapshot.committed_hops==31 && snapshot.state==ORES_FLOW_COMPLETED);
    assert(ores_flow_retire(table,id)==ORES_FLOW_OK);
}
typedef struct {
    OresFlowTable* table;
    uint64_t id;
    OresFlowTicket ticket;
    OresFlowError result;
} Race;
static void* publish_racer(void* arg) {
    Race* r = (Race*)arg;
    r->result = ores_flow_publish(r->table, &r->ticket, accept, NULL);
    return NULL;
}
static void* cancel_racer(void* arg) {
    Race* r = (Race*)arg;
    assert(ores_flow_cancel(r->table, r->id, stages[0])==ORES_FLOW_OK);
    return NULL;
}
static unsigned cancellation_races(OresFlowTable* table) {
    unsigned admitted = 0;
    for (unsigned iteration=0; iteration<200; ++iteration) {
        Race race={0};
        race.table=table;
        assert(ores_flow_open(table,17,3,stages,3,&race.id)==ORES_FLOW_OK);
        assert(ores_flow_reserve(table,race.id,(OresFlowActor){0,0},
               ORES_FLOW_ROOT,0,&race.ticket)==ORES_FLOW_OK);
        pthread_t send_thread,cancel_thread;
        assert(pthread_create(&send_thread,NULL,publish_racer,&race)==0);
        assert(pthread_create(&cancel_thread,NULL,cancel_racer,&race)==0);
        assert(pthread_join(send_thread,NULL)==0);
        assert(pthread_join(cancel_thread,NULL)==0);
        OresFlowSnapshot snap;
        assert(ores_flow_snapshot(table,race.id,&snap)==ORES_FLOW_OK);
        assert(snap.state==ORES_FLOW_CANCELLED && !snap.reserved);
        if(race.result==ORES_FLOW_OK) {
            assert(snap.committed_hops==1);
            ++admitted;
        } else {
            assert(race.result==ORES_FLOW_TERMINAL);
            assert(snap.committed_hops==0);
        }
        assert(ores_flow_retire(table,race.id)==ORES_FLOW_OK);
    }
    return admitted;
}

/* A blocked scheduler admission for flow A must not block unrelated flow B,
 * including its cancellation. Retiring the currently publishing flow returns
 * BUSY instead of taking a global lock and stalling the other flow. */
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int entered;
    int release;
    OresFlowTable* table;
    uint64_t id;
    OresFlowTicket ticket;
    OresFlowError result;
} BlockedPublish;
static int wait_for_test_release(void* ctx, const OresFlowTicket* ticket) {
    BlockedPublish* test = ctx;
    assert(test->ticket.message_id == ticket->message_id);
    assert(pthread_mutex_lock(&test->mutex) == 0);
    test->entered = 1;
    assert(pthread_cond_broadcast(&test->cond) == 0);
    while (!test->release)
        assert(pthread_cond_wait(&test->cond, &test->mutex) == 0);
    assert(pthread_mutex_unlock(&test->mutex) == 0);
    return 1;
}
static void* blocked_sender(void* ctx) {
    BlockedPublish* test = ctx;
    test->result = ores_flow_publish(test->table, &test->ticket,
                                     wait_for_test_release, test);
    return NULL;
}
static int reentrant_publisher(void* ctx, const OresFlowTicket* ticket) {
    OresFlowTable* table = ctx;
    OresFlowSnapshot snapshot;
    assert(ores_flow_snapshot(table, ticket->flow_id, &snapshot)
           == ORES_FLOW_REENTRANT);
    assert(ores_flow_cancel(table, ticket->flow_id, stages[0])
           == ORES_FLOW_REENTRANT);
    assert(ores_flow_retire(table, ticket->flow_id)
           == ORES_FLOW_REENTRANT);
    return 1;
}
static void slow_publisher_isolation(OresFlowTable* table) {
    BlockedPublish test = {0};
    test.table = table;
    assert(pthread_mutex_init(&test.mutex, NULL) == 0);
    assert(pthread_cond_init(&test.cond, NULL) == 0);
    assert(ores_flow_open(table, 17, 3, stages, 3, &test.id) == ORES_FLOW_OK);
    assert(ores_flow_reserve(table, test.id, (OresFlowActor){0,0},
                ORES_FLOW_ROOT, 0, &test.ticket) == ORES_FLOW_OK);
    pthread_t thread;
    assert(pthread_create(&thread, NULL, blocked_sender, &test) == 0);
    assert(pthread_mutex_lock(&test.mutex) == 0);
    while (!test.entered)
        assert(pthread_cond_wait(&test.cond, &test.mutex) == 0);
    assert(pthread_mutex_unlock(&test.mutex) == 0);

    /* These calls would deadlock if try_publish held table.mutex. */
    uint64_t other;
    OresFlowTicket ticket;
    assert(ores_flow_open(table, 17, 3, stages, 3, &other) == ORES_FLOW_OK);
    assert(ores_flow_reserve(table, other, (OresFlowActor){0,0},
                ORES_FLOW_ROOT, 0, &ticket) == ORES_FLOW_OK);
    assert(ores_flow_cancel(table, other, stages[0]) == ORES_FLOW_OK);
    assert(ores_flow_retire(table, other) == ORES_FLOW_OK);
    assert(ores_flow_retire(table, test.id) == ORES_FLOW_BUSY);

    assert(pthread_mutex_lock(&test.mutex) == 0);
    test.release = 1;
    assert(pthread_cond_signal(&test.cond) == 0);
    assert(pthread_mutex_unlock(&test.mutex) == 0);
    assert(pthread_join(thread, NULL) == 0);
    assert(test.result == ORES_FLOW_OK);
    assert(ores_flow_cancel(table, test.id, stages[0]) == ORES_FLOW_OK);
    assert(ores_flow_retire(table, test.id) == ORES_FLOW_OK);
    assert(pthread_cond_destroy(&test.cond) == 0);
    assert(pthread_mutex_destroy(&test.mutex) == 0);
}
static void reentrant_and_recreated_table(OresFlowTable* table) {
    uint64_t old_id, new_id;
    OresFlowTicket ticket;
    assert(ores_flow_open(table, 17, 3, stages, 3, &old_id) == ORES_FLOW_OK);
    assert(ores_flow_reserve(table, old_id, (OresFlowActor){0,0},
        ORES_FLOW_ROOT, 0, &ticket) == ORES_FLOW_OK);
    assert(ores_flow_publish(table, &ticket, reentrant_publisher, table)
           == ORES_FLOW_OK);
    assert(ores_flow_cancel(table, old_id, stages[0]) == ORES_FLOW_OK);
    assert(ores_flow_retire(table, old_id) == ORES_FLOW_OK);
    OresFlowTable* second = ores_flow_table_create();
    assert(second);
    assert(ores_flow_open(second, 17, 3, stages, 3, &new_id) == ORES_FLOW_OK);
    assert(new_id != old_id);
    assert(ores_flow_publish(second, &ticket, accept, NULL)
           == ORES_FLOW_NOT_FOUND);
    assert(ores_flow_cancel(second, new_id, stages[0]) == ORES_FLOW_OK);
    assert(ores_flow_retire(second, new_id) == ORES_FLOW_OK);
    ores_flow_table_destroy(second);
}
static void reject_restarted_duplicate_stage(OresFlowTable* table) {
    OresFlowActor incompatible[3] = {{100, 10}, {100, 11}, {102, 10}};
    uint64_t flow;
    assert(ores_flow_open(table, 17, 3, incompatible, 3, &flow)
           == ORES_FLOW_BAD_ARGUMENT);
}

int main(void) {
    OresFlowTable* table = ores_flow_table_create();
    assert(table);
    basic_and_adversarial(table);
    reject_restarted_duplicate_stage(table);
    slow_publisher_isolation(table);
    reentrant_and_recreated_table(table);
    rejection_and_incomplete(table);
    capacity(table);
    sixteen_stages(table);
    unsigned race_admitted = cancellation_races(table);
    Worker workers[8] = {0};
    pthread_t threads[8];
    for (unsigned t = 0; t < 8; ++t) {
        workers[t].table = table;
        for (unsigned j = 0; j < 10; ++j)
            assert(ores_flow_open(table, 17, 3, stages, 3,
                &workers[t].ids[j]) == ORES_FLOW_OK);
    }
    for (unsigned t = 0; t < 8; ++t)
        assert(pthread_create(&threads[t], NULL, worker, &workers[t]) == 0);
    for (unsigned t = 0; t < 8; ++t) {
        assert(pthread_join(threads[t], NULL) == 0);
        for (unsigned j = 0; j < 10; ++j)
            assert(ores_flow_retire(table, workers[t].ids[j]) == ORES_FLOW_OK);
    }
    assert(atomic_load(&delivered) == 5 + 8 * 10 * 5 + race_admitted);
    ores_flow_table_destroy(table);
    puts("SOURCE_PIPELINE_CORRELATED_FLOWS_PASS flows=80 hops=400 race_attempts=200 isolation=1 terminal=1 generation=1 slow_publish=1 reentry=1");
    return 0;
}
