/* Guests written the way compiled code must behave: every access is cage + u32 offset.
 * A few deliberately break that rule, to show what the rule is protecting. */
#include "oreslang/runtime/TenantVmGuestAbi.h"
#include "guests.h"

#include <stddef.h>

#define AT ORES_VM_AT
#define WRITE_I64(cage, offset, value) (*AT(int64_t, cage, offset) = (value))
#define READ_I64(cage, offset) (*AT(int64_t, cage, offset))

/* KIND_COUNTER: state += input; reply with the total. */
static int64_t counter(const OresVmHostApi* h, void* inst, uint8_t* cage, uint64_t self, uint32_t state, uint32_t in,
                       uint32_t in_len, uint32_t out, uint32_t cap) {
    (void)inst;
    (void)h; (void)self; (void)cap;
    if (in_len != 8) return -2;
    const int64_t total = READ_I64(cage, state) + READ_I64(cage, in);
    WRITE_I64(cage, state, total);
    WRITE_I64(cage, out, total);
    return 8;
}

/* KIND_MEMORY: exercises the actor's heap. Input: op, a, b. */
static int64_t memory(const OresVmHostApi* h, void* inst, uint8_t* cage, uint64_t self, uint32_t state, uint32_t in,
                      uint32_t in_len, uint32_t out, uint32_t cap) {
    (void)inst;
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
/* A host call at every depth, so that one of them is what reaches the end of the stack. */
static int recurse_in_host_calls(const OresVmHostApi* h, int n) {
    volatile char frame[64];
    frame[0] = (char)n;
    const uint64_t actor = h->spawn(KIND_FAULT);
    if (actor) (void)h->stop(actor);
    return recurse_in_host_calls(h, n + 1) + frame[0];
}
static int64_t fault(const OresVmHostApi* h, void* inst, uint8_t* cage, uint64_t self, uint32_t state, uint32_t in,
                     uint32_t in_len, uint32_t out, uint32_t cap) {
    (void)inst;
    (void)self; (void)state; (void)in_len; (void)cap;
    volatile int zero = 0;
    switch (READ_I64(cage, in)) {
        case FAULT_NULL_OFFSET: return READ_I64(cage, 0);
        case FAULT_SPIN: for (;;) {} /* no calls and no checks */
        case FAULT_SPIN_IN_HOST_CALLS: for (;;) (void)h->alloc(1, 1);
        case FAULT_STACK: return recurse(0);
        case FAULT_STACK_IN_HOST_CALLS: return recurse_in_host_calls(h, 0);
        case FAULT_TRAP: __builtin_trap();
        case FAULT_DIVIDE: WRITE_I64(cage, out, 1 / zero); return 8;
        case FAULT_SLEEPLESS_BRIEFLY: { volatile uint64_t i = 0; while (i < 20000000u) i = i + 1; return 0; }
        default: return -3;
    }
}

/* KIND_RELAY: forwards a counter increment to another actor. Input: target, delta. */
static int64_t relay(const OresVmHostApi* h, void* inst, uint8_t* cage, uint64_t self, uint32_t state, uint32_t in,
                     uint32_t in_len, uint32_t out, uint32_t cap) {
    (void)inst;
    (void)self; (void)state; (void)in_len; (void)cap;
    const uint64_t target = (uint64_t)READ_I64(cage, in);
    WRITE_I64(cage, out, h->send(target, in + 8, 8));
    return 8;
}

/* KIND_SPAWNER: starts an actor of the requested kind, replies with its ref. */
static int64_t spawner(const OresVmHostApi* h, void* inst, uint8_t* cage, uint64_t self, uint32_t state, uint32_t in,
                       uint32_t in_len, uint32_t out, uint32_t cap) {
    (void)inst;
    (void)self; (void)state; (void)in_len; (void)cap;
    WRITE_I64(cage, out, (int64_t)h->spawn((uint32_t)READ_I64(cage, in)));
    return 8;
}

/* KIND_HOSTILE: host calls with arguments chosen by the input, exactly as a compromised actor
 * would make them. Input: op, a, b, c, then for HOSTILE_SCRIPT a list of (call, x, y, z). */
static int64_t hostile(const OresVmHostApi* h, void* inst, uint8_t* cage, uint64_t self, uint32_t state, uint32_t in,
                       uint32_t in_len, uint32_t out, uint32_t cap) {
    (void)inst; (void)state; (void)cap;
    const int64_t op = READ_I64(cage, in), a = READ_I64(cage, in + 8), b = READ_I64(cage, in + 16), c = READ_I64(cage, in + 24);
    int64_t value = 0;
    switch (op) {
        case HOSTILE_SEND: value = h->send(a == -1 ? self : (uint64_t)a, (uint32_t)b, (uint32_t)c); break;
        case HOSTILE_FLOOD_SEND: while (h->send((uint64_t)a, in, (uint32_t)b)) ++value; break;
        case HOSTILE_SPAWN: value = (int64_t)h->spawn((uint32_t)a); break;
        case HOSTILE_ALLOC: value = h->alloc((uint32_t)a, (uint32_t)b); break;
        case HOSTILE_FREE: value = h->free((uint32_t)a, (uint32_t)b, (uint32_t)c); break;
        case HOSTILE_ALLOC_FREE_LOOP:
            for (int64_t i = 0; i < a; ++i) {
                const uint32_t block = h->alloc((uint32_t)b, 16);
                if (!block) return -4;
                WRITE_I64(cage, block, i);
                if (!h->free(block, (uint32_t)b, 16)) return -5;
            }
            value = 1;
            break;
        case HOSTILE_POISON_FREE_LIST: { /* free a block, overwrite its link, then allocate through it */
            const uint32_t block = h->alloc(64, 16);
            if (!block || !h->free(block, 64, 16)) return -6;
            *AT(uint32_t, cage, block) = (uint32_t)a;
            const uint32_t first = h->alloc(64, 16);
            const uint32_t second = h->alloc(64, 16);
            WRITE_I64(cage, out, first);
            WRITE_I64(cage, out + 8, second);
            return 16;
        }
        case HOSTILE_SCRIPT: {
            for (uint32_t at = in + 32; at + 32 <= in + in_len; at += 32) {
                const uint64_t x = (uint64_t)READ_I64(cage, at + 8), y = (uint64_t)READ_I64(cage, at + 16), z = (uint64_t)READ_I64(cage, at + 24);
                switch ((uint64_t)READ_I64(cage, at) % 6) {
                    case 5: value += h->stop(x); break;
                    case 0: value += h->alloc((uint32_t)x, (uint32_t)y) != 0; break;
                    case 1: value += h->free((uint32_t)x, (uint32_t)y, (uint32_t)z); break;
                    case 2: value += h->send(x, (uint32_t)y, (uint32_t)z); break;
                    case 3: value += h->send(self, (uint32_t)y, (uint32_t)z); break;
                    default: value += h->spawn((uint32_t)x) != 0; break;
                }
            }
            break;
        }
        default: return -3;
    }
    WRITE_I64(cage, out, value);
    return 8;
}

/* KIND_TINY_SCRATCH: claims the largest reply the host offered, touching nothing. */
static int64_t tiny(const OresVmHostApi* h, void* inst, uint8_t* cage, uint64_t self, uint32_t state, uint32_t in,
                    uint32_t in_len, uint32_t out, uint32_t cap) {
    (void)h; (void)inst; (void)cage; (void)self; (void)state; (void)in; (void)in_len; (void)out;
    return cap;
}

/* KIND_BURN: runs for as many iterations as asked, so turns can differ in length. */
static int64_t burn(const OresVmHostApi* h, void* inst, uint8_t* cage, uint64_t self, uint32_t state, uint32_t in,
                    uint32_t in_len, uint32_t out, uint32_t cap) {
    (void)h; (void)inst; (void)self; (void)state; (void)in_len; (void)out; (void)cap;
    volatile uint64_t i = 0;
    const uint64_t n = (uint64_t)READ_I64(cage, in);
    while (i < n) i = i + 1;
    return 0;
}

/* KIND_INSTANCE: state kept outside the cage. Counts in it, and reports the first eight bytes. */
static int64_t instance(const OresVmHostApi* h, void* inst, uint8_t* cage, uint64_t self, uint32_t state, uint32_t in,
                        uint32_t in_len, uint32_t out, uint32_t cap) {
    (void)h; (void)self; (void)state; (void)in; (void)in_len; (void)cap;
    WRITE_I64(cage, out, ++*(int64_t*)inst);
    return 8;
}

/* KIND_RESTARTABLE: as a lowering that proves confinement would behave. Keeps a counter in the cage, which
 * a restart clears, and commits it to stored state, which a restart keeps. Input: op, a. */
static int64_t restartable(const OresVmHostApi* h, void* inst, uint8_t* cage, uint64_t self, uint32_t state, uint32_t in,
                           uint32_t in_len, uint32_t out, uint32_t cap) {
    (void)self; (void)cap; (void)in_len;
    int64_t* stored = (int64_t*)((char*)inst + 16);
    int64_t* ready = (int64_t*)inst;
    if (!*ready) { /* a fresh or restarted actor: load what was committed */
        WRITE_I64(cage, state, *stored);
        *ready = 1;
    }
    const int64_t op = READ_I64(cage, in), a = READ_I64(cage, in + 8);
    switch (op) {
        case RESTART_ADD:
            WRITE_I64(cage, state, READ_I64(cage, state) + a);
            *stored = READ_I64(cage, state);
            break;
        case RESTART_ADD_THEN_TRAP: /* not committed, so a restart must not keep it */
            WRITE_I64(cage, state, READ_I64(cage, state) + a);
            h->trap();
            break;
        case RESTART_SPIN: for (;;) {}
        case RESTART_SPIN_IN_HOST_CALLS: for (;;) (void)h->alloc(1, 1);
        case RESTART_STACK: return recurse(0);
        case RESTART_BAD_READ: return READ_I64(cage, 0); /* what bounds checks should have prevented */
        case RESTART_READ_HEAP_OK: { /* a restart must give back the heap */
            const uint32_t block = h->alloc(4096, 16);
            WRITE_I64(cage, out, block != 0);
            return 8;
        }
        default: break;
    }
    WRITE_I64(cage, out, READ_I64(cage, state));
    return 8;
}

static const OresVmGuestKind kinds[] = {
    [KIND_COUNTER] = {counter, 16, 0, 64, 0, 0, 0},
    [KIND_MEMORY] = {memory, 64, 4096, 256, 0, 0, 0},
    [KIND_FAULT] = {fault, 16, 0, 64, 0, 0, 0},
    [KIND_RELAY] = {relay, 16, 0, 64, 0, 0, 0},
    [KIND_SPAWNER] = {spawner, 16, 0, 64, 0, 0, 0},
    [KIND_LARGE] = {counter, 1u << 20, 0, 64, 0, 0, 0},
    [KIND_HOSTILE] = {hostile, 64, 1u << 16, 4096, 0, 0, 0},
    [KIND_HOSTILE_LARGE_MAIL] = {hostile, 16, 0, 131072, 0, 0, 0},
    /* Ends exactly at a commit boundary with too little scratch for an aligned output. */
    [KIND_TINY_SCRATCH] = {tiny, (2u << 20) - 12, 0, 8, 0, 0, 0},
    [KIND_BURN] = {burn, 16, 0, 64, 0, 0, 0},
    [KIND_INSTANCE] = {instance, 16, 0, 64, 64, 0, 0},
    [KIND_RESTARTABLE] = {restartable, 16, 8192, 64, 16, 8, ORES_VM_KIND_RESTARTABLE},
    [KIND_RESTART_UNFLAGGED] = {restartable, 16, 8192, 64, 16, 8, 0},
};
const OresVmGuestModule ores_vm_test_module = {ORES_VM_GUEST_ABI_VERSION, sizeof kinds / sizeof kinds[0], kinds};
