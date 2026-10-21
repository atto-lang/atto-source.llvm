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
     * This target compiles NativeActorScheduler.c with
     * ORESLANG_NATIVE_TEST_CALLOC_FAIL_ORDINAL=6.
     *
     * Allocation order in open_typed_control is:
     *   1 scheduler
     *   2 actors
     *   3 message slots
     *   4 worker thread ids
     *   5 ready_control_bits
     *   6 ready_data_bits  <-- forced NULL
     *
     * The cleanup path therefore owns ready_control_bits but not
     * ready_data_bits. A duplicated free of the readiness arrays aborts
     * here (and is also caught by ASan in the sanitizer carrier run).
     */
    OresNativeActorScheduler* scheduler = ores_native_actor_scheduler_open(
        16, 2, 1, on_data, NULL);
    assert(scheduler == NULL);
    puts("NATIVE_ACTOR_ALLOCATION_FAILURE_PASS ordinal=6 cleanup_exactly_once=1");
    return 0;
}
