#include "oreslang/runtime/SourceActorBootstrap.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>

static atomic_int initialized;
static atomic_int messages;
static atomic_int resumes;
static pthread_t producer;
static void* expected_actor;

static void require(int ok) { if (!ok) abort(); }

static void init_actor(OresActorI64Heap* heap, uint64_t domain, uint64_t epoch) {
    require(heap != NULL && domain != 0 && epoch != 0);
    require(!pthread_equal(pthread_self(), producer));
    atomic_fetch_add(&initialized, 1);
}

static int64_t on_message(
        OresActorI64Heap* heap, uint64_t domain, uint64_t epoch, int64_t payload) {
    require(heap != NULL && domain != 0 && epoch != 0);
    require(!pthread_equal(pthread_self(), producer));
    require(payload == 41);
    atomic_fetch_add(&messages, 1);
    return payload + 1;
}

static void on_capability(
        void* actor, OresActorI64Heap* heap, uint64_t domain, uint64_t epoch,
        OresNativeActorCapabilityWake wake) {
    require(actor == expected_actor);
    require(heap != NULL && domain != 0 && epoch != 0);
    require(!pthread_equal(pthread_self(), producer));
    require(wake.scope == 71 && wake.ticket == 72);
    require(wake.status == ORES_NATIVE_CAPABILITY_READY);
    atomic_fetch_add(&resumes, 1);
}

int main(void) {
    producer = pthread_self();

    /* Legacy synchronous source actors cannot be targeted by the continuation
     * wake API. Preserve the existing ABI and fail closed. */
    void* sync = oreslang_source_actor_spawn(init_actor, on_message, 1);
    require(sync != NULL);
    OresNativeActorCapabilityWake wake = {71, 72, ORES_NATIVE_CAPABILITY_READY};
    require(!oreslang_source_actor_publish_capability_wake(sync, wake));
    oreslang_source_actor_send(sync, 41);
    oreslang_source_actor_stop(sync);
    require(atomic_load(&initialized) == 1);
    require(atomic_load(&messages) == 1);
    require(atomic_load(&resumes) == 0);

    atomic_store(&initialized, 0);
    atomic_store(&messages, 0);

    void* actor = oreslang_source_actor_spawn_resumable(
        init_actor, on_message, on_capability, 1);
    require(actor != NULL);
    expected_actor = actor;

    OresNativeActorCapabilityWake malformed = {0, 72, ORES_NATIVE_CAPABILITY_READY};
    require(!oreslang_source_actor_publish_capability_wake(actor, malformed));
    malformed = (OresNativeActorCapabilityWake){71, 72, 99};
    require(!oreslang_source_actor_publish_capability_wake(actor, malformed));

    oreslang_source_actor_send(actor, 41);
    require(oreslang_source_actor_publish_capability_wake(actor, wake));
    oreslang_source_actor_stop(actor);

    require(atomic_load(&initialized) == 1);
    require(atomic_load(&messages) == 1);
    require(atomic_load(&resumes) == 1);
    return 0;
}
