/* Guests written the way compiled code must behave: every access is cage + u32 offset.
 * A few deliberately break that rule, to show what the rule is protecting. */
#include "oreslang/runtime/TenantVmGuestAbi.h"
#include "guests.h"

#include <stddef.h>

#define AT ORES_VM_AT
#define WRITE_I64(cage, offset, value) (*AT(int64_t, cage, offset) = (value))
#define READ_I64(cage, offset) (*AT(int64_t, cage, offset))

/* KIND_COUNTER: state += input; reply with the total. */
static int64_t counter(const OresVmHostApi* h, uint8_t* cage, uint64_t self, uint32_t state, uint32_t in,
                       uint32_t in_len, uint32_t out, uint32_t cap) {
    (void)h; (void)self; (void)cap;
    if (in_len != 8) return -2;
    const int64_t total = READ_I64(cage, state) + READ_I64(cage, in);
    WRITE_I64(cage, state, total);
    WRITE_I64(cage, out, total);
    return 8;
}

/* KIND_MEMORY: exercises the actor's heap. Input: op, a, b. */
static int64_t memory(const OresVmHostApi* h, uint8_t* cage, uint64_t self, uint32_t state, uint32_t in,
                      uint32_t in_len, uint32_t out, uint32_t cap) {
    (void)self; (void)cap; (void)in_len;
    const int64_t op = READ_I64(cage, in), a = READ_I64(cage, in + 8), b = READ_I64(cage, in + 16);
    int64_t value = 0;
    switch (op) {
        case MEM_WRITE_STATE: WRITE_I64(cage, state, a); break;
        case MEM_READ_STATE: value = READ_I64(cage, state); break;
        case MEM_STATE_OFFSET: value = state; break;
        case MEM_READ_OFFSET: value = READ_I64(cage, a); break;
        case MEM_WRITE_OFFSET: WRITE_I64(cage, a, b); break;
        case MEM_ALLOC: value = h->alloc((uint32_t)a, (uint32_t)b); break;
        case MEM_ALLOC_UNTIL_REFUSED: {
            while (h->alloc((uint32_t)a, 16)) ++value;
            break;
        }
        /* The addressing rule itself: the largest offset plus a large displacement. */
        case MEM_FARTHEST_ACCESS: value = *(volatile int64_t*)(cage + (uint64_t)(uint32_t)0xFFFFFFFFu + 0x7FFFFFFFu); break;
        /* Breaks the rule, as hostile or miscompiled code would. */
        case MEM_RULE_BREAKING_READ: value = *(volatile int64_t*)(uintptr_t)a; break;
        default: return -3;
    }
    WRITE_I64(cage, out, value);
    return 8;
}

/* KIND_FAULT: ends its VM some other way. */
static int recurse(int n) {
    volatile char frame[1024];
    frame[0] = (char)n;
    return recurse(n + 1) + frame[0];
}
static int64_t fault(const OresVmHostApi* h, uint8_t* cage, uint64_t self, uint32_t state, uint32_t in,
                     uint32_t in_len, uint32_t out, uint32_t cap) {
    (void)self; (void)state; (void)in_len; (void)cap;
    volatile int zero = 0;
    switch (READ_I64(cage, in)) {
        case FAULT_NULL_OFFSET: return READ_I64(cage, 0);
        case FAULT_SPIN: for (;;) {} /* no calls and no checks */
        case FAULT_SPIN_IN_HOST_CALLS: for (;;) (void)h->alloc(1, 1);
        case FAULT_STACK: return recurse(0);
        case FAULT_TRAP: __builtin_trap();
        case FAULT_DIVIDE: WRITE_I64(cage, out, 1 / zero); return 8;
        case FAULT_SLEEPLESS_BRIEFLY: { volatile uint64_t i = 0; while (i < 20000000u) i = i + 1; return 0; }
        default: return -3;
    }
}

/* KIND_RELAY: forwards a counter increment to another actor. Input: target, delta. */
static int64_t relay(const OresVmHostApi* h, uint8_t* cage, uint64_t self, uint32_t state, uint32_t in,
                     uint32_t in_len, uint32_t out, uint32_t cap) {
    (void)self; (void)state; (void)in_len; (void)cap;
    const uint64_t target = (uint64_t)READ_I64(cage, in);
    WRITE_I64(cage, out, h->send(target, in + 8, 8));
    return 8;
}

/* KIND_SPAWNER: starts an actor of the requested kind, replies with its ref. */
static int64_t spawner(const OresVmHostApi* h, uint8_t* cage, uint64_t self, uint32_t state, uint32_t in,
                       uint32_t in_len, uint32_t out, uint32_t cap) {
    (void)self; (void)state; (void)in_len; (void)cap;
    WRITE_I64(cage, out, (int64_t)h->spawn((uint32_t)READ_I64(cage, in)));
    return 8;
}

static const OresVmGuestKind kinds[] = {
    [KIND_COUNTER] = {counter, 16, 0, 64},
    [KIND_MEMORY] = {memory, 64, 4096, 256},
    [KIND_FAULT] = {fault, 16, 0, 64},
    [KIND_RELAY] = {relay, 16, 0, 64},
    [KIND_SPAWNER] = {spawner, 16, 0, 64},
    [KIND_LARGE] = {counter, 1u << 20, 0, 64},
};
const OresVmGuestModule ores_vm_test_module = {ORES_VM_GUEST_ABI_VERSION, sizeof kinds / sizeof kinds[0], kinds};
