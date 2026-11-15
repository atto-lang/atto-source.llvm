/*
 * Minimal platform startup for the ores.core bare-metal gate (QEMU lm3s6965evb,
 * Cortex-M3). This file is the *platform*, not Oreslang: it owns the vector
 * table, reset, .data/.bss setup and the failure hook. It must build with
 * -ffreestanding -nostdlib and link with no libc, compiler-rt or RTOS.
 *
 * Results are reported through ARM semihosting (BKPT 0xAB), which QEMU
 * services on the host. Semihosting is test plumbing for this harness only;
 * the Oreslang guest object never calls it.
 *
 * Build modes:
 *   default            call ores_core_entry(5), expect 42, exit success
 *   ORES_PROBE_TRAP    call ores_core_overflow_probe(INT64_MAX); the guest's
 *                      checked multiply must trap into HardFault_Handler
 */
#include <stdint.h>

extern int64_t ores_core_entry(int64_t x);
extern int64_t ores_core_overflow_probe(int64_t x);

extern uint32_t __stack_top;
extern uint32_t __data_load;
extern uint32_t __data_start;
extern uint32_t __data_end;
extern uint32_t __bss_start;
extern uint32_t __bss_end;

#define SYS_WRITE0 0x04u
#define SYS_EXIT 0x18u
#define ADP_STOPPED_APPLICATION_EXIT 0x20026u
#define ADP_STOPPED_RUNTIME_ERROR_UNKNOWN 0x20023u

static uint32_t semihost(uint32_t op, uintptr_t arg) {
    register uint32_t r0 __asm__("r0") = op;
    register uintptr_t r1 __asm__("r1") = arg;
    __asm__ volatile("bkpt 0xab" : "+r"(r0) : "r"(r1) : "memory");
    return r0;
}

static void write0(const char *text) {
    semihost(SYS_WRITE0, (uintptr_t)text);
}

static __attribute__((noreturn)) void exit_with(uint32_t reason) {
    semihost(SYS_EXIT, reason);
    for (;;) {
    }
}

/* Decimal output by repeated subtraction: 64-bit division on Cortex-M3 would
 * need __aeabi_uldivmod from compiler-rt, which this image does not link. */
static void write_i64(int64_t value) {
    static const uint64_t powers[] = {
        10000000000000000000ull, 1000000000000000000ull, 100000000000000000ull,
        10000000000000000ull, 1000000000000000ull, 100000000000000ull,
        10000000000000ull, 1000000000000ull, 100000000000ull, 10000000000ull,
        1000000000ull, 100000000ull, 10000000ull, 1000000ull, 100000ull,
        10000ull, 1000ull, 100ull, 10ull, 1ull,
    };
    char buffer[24];
    volatile char *cursor = buffer;
    uint64_t magnitude = value < 0 ? (uint64_t)0 - (uint64_t)value : (uint64_t)value;
    int started = 0;
    if (value < 0) {
        *cursor++ = '-';
    }
    for (unsigned i = 0; i < sizeof(powers) / sizeof(powers[0]); ++i) {
        char digit = '0';
        while (magnitude >= powers[i]) {
            magnitude -= powers[i];
            ++digit;
        }
        if (digit != '0' || started || powers[i] == 1u) {
            *cursor++ = digit;
            started = 1;
        }
    }
    *cursor = '\0';
    write0(buffer);
}

static void harness_main(void) {
#ifdef ORES_PROBE_TRAP
    write0("ORES_CORE_TRAP_PROBE start\n");
    volatile int64_t input = INT64_MAX;
    int64_t value = ores_core_overflow_probe(input);
    /* Reaching here means overflow did not trap: a semantic failure. */
    write0("ORES_CORE_TRAP_PROBE_FAIL no trap, value=");
    write_i64(value);
    write0("\n");
    exit_with(ADP_STOPPED_RUNTIME_ERROR_UNKNOWN);
#else
    /* Each row drives a different path through app.ores: inside/outside the
     * && / || window, and the lower and upper clamp bounds. */
    static const struct { int64_t input; int64_t expected; } vectors[] = {
        {5, 42}, {0, 7}, {2, 19}, {20, 265}, {-4, 102}, {200, 1300},
    };
    for (unsigned i = 0; i < sizeof(vectors) / sizeof(vectors[0]); ++i) {
        volatile int64_t input = vectors[i].input;
        int64_t value = ores_core_entry(input);
        if (value != vectors[i].expected) {
            write0("ORES_CORE_BAREMETAL_FAIL input=");
            write_i64(vectors[i].input);
            write0(" value=");
            write_i64(value);
            write0(" expected=");
            write_i64(vectors[i].expected);
            write0("\n");
            exit_with(ADP_STOPPED_RUNTIME_ERROR_UNKNOWN);
        }
    }
    write0("ORES_CORE_BAREMETAL_PASS vectors=6 value=");
    write_i64(ores_core_entry(5));
    write0("\n");
    exit_with(ADP_STOPPED_APPLICATION_EXIT);
#endif
}

void Reset_Handler(void) {
    /* Volatile pointers keep the compiler from turning these loops into
     * memcpy/memset calls, which this freestanding image does not provide. */
    volatile uint32_t *src = &__data_load;
    volatile uint32_t *dst = &__data_start;
    while (dst < &__data_end) {
        *dst++ = *src++;
    }
    for (volatile uint32_t *bss = &__bss_start; bss < &__bss_end; ++bss) {
        *bss = 0u;
    }
    harness_main();
    exit_with(ADP_STOPPED_RUNTIME_ERROR_UNKNOWN);
}

/* Platform failure hook. Oreslang checked arithmetic lowers to llvm.trap
 * (UDF), which escalates to HardFault on Cortex-M3 with UsageFault disabled. */
void HardFault_Handler(void) {
    write0("ORES_CORE_TRAP hardfault\n");
    exit_with(ADP_STOPPED_RUNTIME_ERROR_UNKNOWN);
}

void Default_Handler(void) {
    write0("ORES_CORE_UNEXPECTED_EXCEPTION\n");
    exit_with(ADP_STOPPED_RUNTIME_ERROR_UNKNOWN);
}

typedef void (*vector_entry)(void);

__attribute__((section(".isr_vector"), used))
const vector_entry ores_vector_table[16] = {
    (vector_entry)(uintptr_t)&__stack_top,
    Reset_Handler,
    Default_Handler,   /* NMI */
    HardFault_Handler, /* HardFault */
    Default_Handler,   /* MemManage */
    Default_Handler,   /* BusFault */
    Default_Handler,   /* UsageFault */
    0, 0, 0, 0,        /* reserved */
    Default_Handler,   /* SVCall */
    Default_Handler,   /* DebugMonitor */
    0,                 /* reserved */
    Default_Handler,   /* PendSV */
    Default_Handler,   /* SysTick */
};
