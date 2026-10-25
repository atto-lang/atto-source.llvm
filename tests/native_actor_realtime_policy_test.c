#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeActorScheduler.h"
#include <sched.h>
#include <stdint.h>
#include <stdlib.h>

static void require(int ok) { if (!ok) abort(); }
static void unused(void* ctx, size_t actor, uint64_t payload) {
    (void)ctx; (void)actor; (void)payload;
}

int main(void) {
    OresNativeActorScheduler* s =
        ores_native_actor_scheduler_open(1, 1, 2, unused, NULL);
    require(s != NULL);

    int minimum = sched_get_priority_min(SCHED_FIFO);
    int maximum = sched_get_priority_max(SCHED_FIFO);
    require(minimum >= 0 && maximum >= minimum);
    require(ores_native_actor_scheduler_configure_fifo(s, minimum));
    require(!ores_native_actor_scheduler_fifo_carriers_ready(s));
    require(ores_native_actor_scheduler_accepted(s) == 0);

    /* Unprivileged Linux commonly rejects pthread_create with EPERM here.
     * Either outcome is valid: success must mean every carrier is ready;
     * failure must still have admitted zero actor work. */
    int started = ores_native_actor_scheduler_prestart_carriers(s);
    if (started) {
        require(ores_native_actor_scheduler_all_carriers_started(s));
        require(ores_native_actor_scheduler_fifo_carriers_ready(s));
        require(ores_native_actor_scheduler_started_carriers(s) == 2);
    } else {
        require(!ores_native_actor_scheduler_fifo_carriers_ready(s));
        require(ores_native_actor_scheduler_accepted(s) == 0);
        require(ores_native_actor_scheduler_rejected(s) == 0);
    }

    ores_native_actor_scheduler_retire(s);
    require(ores_native_actor_scheduler_drain(s));
    require(ores_native_actor_scheduler_try_destroy(s));
    return 0;
}
