// Test-only freestanding Cortex-M3 startup. Never linked into atto.core.
// Semihosting is a test-harness exit channel, not an AttoLang std/OS dependency.
#include <stdint.h>

extern int main(void); // Emitted from a compiled .atto source.
void Reset_Handler(void);
__attribute__((section(".isr_vector"), used))
const uintptr_t atto_test_vectors[2] = {
    (uintptr_t)0x20010000u, (uintptr_t)&Reset_Handler
};

static __attribute__((noreturn)) void report_to_test_runner(uint32_t status) {
    const uint32_t args[2] = {0x20026u, status};
    register uint32_t r0 __asm__("r0") = 0x20u; // SYS_EXIT_EXTENDED
    register const uint32_t *r1 __asm__("r1") = args;
    __asm__ volatile ("bkpt #0xab" : "+r"(r0) : "r"(r1) : "memory");
    for (;;) { __asm__ volatile ("wfi"); }
}

void Reset_Handler(void) {
    // This harness expects the actual Atto main to return 42, not an
    // unrelated linkable object or a generated host-language stub.
    report_to_test_runner(main() == 42 ? 0u : 1u);
}
