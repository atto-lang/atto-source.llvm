/* Standalone Cortex-M kernel integration fixture. C handlers, NOT Oreslang actor source.
 * This file uses only integer arithmetic and the freestanding actor kernel. */
#include "oreslang/runtime/FreestandingActorKernel.h"
static OresKernel primary;
static OresKernel foreign;
static volatile uint32_t bss_witness;
static volatile uint32_t data_witness = UINT32_C(0x75ab9321);
static int sum_handler(uint64_t fields[ORES_KERNEL_FIELDS], int64_t message, int64_t *result) {
    fields[0] += (uint64_t)message;
    *result = (int64_t)fields[0];
    return 1;
}
static int fault_handler(uint64_t fields[ORES_KERNEL_FIELDS], int64_t message, int64_t *result) {
    if (message == -1) return 0;
    return sum_handler(fields, message, result);
}
int firmware_main(void) {
    OresKernelRef a, b, wrong, fresh;
    uint64_t processed;
    int64_t result;
    if (bss_witness != 0 || data_witness != UINT32_C(0x75ab9321)) return 1;
    if (ores_kernel_init(&primary) != ORES_KERNEL_OK ||
        ores_kernel_init(&foreign) != ORES_KERNEL_OK) return 2;
    if (ores_kernel_spawn(&primary, sum_handler, &a) != ORES_KERNEL_OK ||
        ores_kernel_spawn(&primary, sum_handler, &b) != ORES_KERNEL_OK ||
        ores_kernel_spawn(&foreign, sum_handler, &wrong) != ORES_KERNEL_OK) return 3;
    if (wrong.slot != a.slot || wrong.generation != a.generation ||
        ores_kernel_send(&primary, wrong, 8) != ORES_KERNEL_INVALID ||
        ores_kernel_stop(&foreign, a) != ORES_KERNEL_INVALID) return 4;
    if (ores_kernel_send(&primary, a, 2) != ORES_KERNEL_OK ||
        ores_kernel_send(&primary, a, 3) != ORES_KERNEL_OK ||
        ores_kernel_send(&primary, b, 11) != ORES_KERNEL_OK ||
        ores_kernel_stop(&primary, a) != ORES_KERNEL_OK ||
        ores_kernel_send(&primary, a, 4) != ORES_KERNEL_CLOSED) return 5;
    for (unsigned i=0;i<3;i++)
        if (ores_kernel_tick(&primary) != ORES_KERNEL_DELIVERED) return 6;
    if (ores_kernel_snapshot(&primary, a, &processed, &result) != ORES_KERNEL_OK ||
        processed != 2 || result != 5 ||
        ores_kernel_snapshot(&primary, b, &processed, &result) != ORES_KERNEL_OK ||
        processed != 1 || result != 11) return 7;
    if (ores_kernel_tick(&primary) != ORES_KERNEL_RETIRED ||
        ores_kernel_send(&primary, a, 1) != ORES_KERNEL_INVALID) return 8;
    for (unsigned i=0;i<ORES_KERNEL_MAILBOX;i++)
        if (ores_kernel_send(&primary, b, 1) != ORES_KERNEL_OK) return 9;
    if (ores_kernel_send(&primary, b, 1) != ORES_KERNEL_FULL) return 10;
    if (ores_kernel_stop(&primary, b) != ORES_KERNEL_OK) return 11;
    for (unsigned i=0;i<ORES_KERNEL_MAILBOX;i++)
        if (ores_kernel_tick(&primary) != ORES_KERNEL_DELIVERED) return 12;
    if (ores_kernel_snapshot(&primary, b, &processed, &result) != ORES_KERNEL_OK ||
        processed != ORES_KERNEL_MAILBOX + 1 || result != 27 ||
        ores_kernel_tick(&primary) != ORES_KERNEL_RETIRED) return 13;
    if (ores_kernel_spawn(&primary, fault_handler, &fresh) != ORES_KERNEL_OK ||
        ores_kernel_send(&primary, fresh, -1) != ORES_KERNEL_OK ||
        ores_kernel_tick(&primary) != ORES_KERNEL_HANDLER_FAILED ||
        primary.fault_count != 1 ||
        ores_kernel_send(&primary, fresh, 1) != ORES_KERNEL_INVALID) return 14;
    if (ores_kernel_init(&primary) != ORES_KERNEL_OK ||
        ores_kernel_spawn(&primary, sum_handler, &fresh) != ORES_KERNEL_OK ||
        ores_kernel_send(&primary, a, 1) != ORES_KERNEL_INVALID) return 15;
    primary.actors[fresh.slot].head = ORES_KERNEL_MAILBOX + 4;
    if (ores_kernel_send(&primary, fresh, 1) != ORES_KERNEL_INVALID ||
        ores_kernel_tick(&primary) != ORES_KERNEL_HANDLER_FAILED ||
        primary.fault_count != 1) return 16;
#ifdef NEGATIVE_GUEST
    return 99; /* Negative control must take the failure trap. */
#endif
    return 0;
}
