#include "oreslang/runtime/NativeActorScheduler.h"
#if defined(NDEBUG)
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static void on_data(void* context, size_t actor, uint64_t payload) {
    (void)context;
    (void)actor;
    (void)payload;
}

int main(void) {
    /*
     * open_typed_control performs eight scheduler-owned calloc calls:
     *   1 scheduler
     *   2 actors
     *   3 message slots
     *   4 worker thread ids
     *   5 ready_control_bits
     *   6 ready_data_bits
     *   7 summary_control_bits
     *   8 summary_data_bits
     *
     * Exercise every partial-allocation boundary in one executable. The
     * sanitizer carrier run turns leaks/double-free/use-after-free into a
     * hard failure without consuming additional reviewed plan commands.
     */
    for (unsigned ordinal = 1; ordinal <= 8; ++ordinal) {
        ores_native_actor_scheduler_test_fail_calloc_ordinal(ordinal);
        OresNativeActorScheduler* scheduler = ores_native_actor_scheduler_open(
            16, 2, 1, on_data, NULL);
        assert(scheduler == NULL);
    }

    puts("NATIVE_ACTOR_ALLOCATION_FAILURE_PASS ordinals=1..8 cleanup_exactly_once=1");
    return 0;
}
