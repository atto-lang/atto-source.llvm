/* Runs one Wasm module, lowered by wasm2c, as a tenant VM guest. Compiled once per module.
 *
 * Each actor gets its own Wasm instance. Its linear memory is the actor's region of the cage, so
 * actors of one module cannot collide, and fields, data and stack persist across turns. The
 * instance itself (Wasm globals, table, memory descriptor) sits in `instance` memory outside
 * the cage, where guest load and store instructions cannot name it.
 *
 * No malloc, no thread-locals, nothing that takes a lock: a kill can land anywhere in here.
 * A thread-local in a dlopen'ed library can allocate on first touch.
 */
#include "oreslang/runtime/TenantVmGuestAbi.h"
#include "guest.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef ORES_VM_TABLE_SLOTS
#define ORES_VM_TABLE_SLOTS 0
#endif

struct w2c_env { wasm_rt_memory_t* memory; };

typedef struct {
    w2c_guest instance;
    wasm_rt_memory_t memory;
    struct w2c_env env;
#if ORES_VM_TABLE_SLOTS
    wasm_rt_funcref_t table[ORES_VM_TABLE_SLOTS];
#endif
    uint32_t ready;
} Blob;

/* The same value for every turn of a host: its host API. Never reachable from guest code. */
static const OresVmHostApi* g_host;

/* What generated code needs from a Wasm runtime, reduced to this VM's behaviour. */
void wasm_rt_trap(wasm_rt_trap_t code) {
    (void)code;
    __atomic_load_n(&g_host, __ATOMIC_RELAXED)->trap();
    __builtin_unreachable();
}
bool wasm_rt_is_initialized(void) { return true; }
void wasm_rt_init(void) {}
uint64_t wasm_rt_grow_memory(wasm_rt_memory_t* memory, uint64_t pages) {
    (void)memory;
    (void)pages;
    return (uint64_t)-1; /* an actor's memory is fixed by its kind */
}
void wasm_rt_allocate_funcref_table(wasm_rt_funcref_table_t* table, uint32_t elements, uint32_t max_elements) {
#if ORES_VM_TABLE_SLOTS
    /* The table field lives inside its blob, and it is the only way to find the blob here. */
    Blob* blob = (Blob*)((char*)table - offsetof(Blob, instance.w2c_T0));
    if (elements > ORES_VM_TABLE_SLOTS) wasm_rt_trap(WASM_RT_TRAP_OOB);
    memset(blob->table, 0, sizeof blob->table);
    table->data = blob->table;
    table->size = elements;
    table->max_size = max_elements;
#else
    (void)table;
    (void)elements;
    (void)max_elements;
    wasm_rt_trap(WASM_RT_TRAP_OOB);
#endif
}
void wasm_rt_free_funcref_table(wasm_rt_funcref_table_t* table) { (void)table; }
wasm_rt_memory_t* w2c_env_memory(struct w2c_env* env) { return env->memory; }

static int64_t turn(const OresVmHostApi* host, void* raw, uint8_t* cage, uint64_t self, uint32_t state, uint32_t input,
                    uint32_t input_length, uint32_t output, uint32_t capacity) {
    if (__atomic_load_n(&g_host, __ATOMIC_RELAXED) != host) __atomic_store_n(&g_host, host, __ATOMIC_RELAXED);
    Blob* blob = (Blob*)raw;
    if (!blob->ready) {
        blob->memory.data = cage + state;
        blob->memory.pages = ORES_VM_MEMORY_BYTES / 65536;
        blob->memory.max_pages = ORES_VM_MEMORY_BYTES / 65536;
        /* The whole region: linear memory, then this turn's scratch. */
        blob->memory.size = ORES_VM_MEMORY_BYTES + ORES_VM_SCRATCH_BYTES;
        blob->env.memory = &blob->memory;
        wasm2c_guest_instantiate(&blob->instance, &blob->env);
#if defined(ORES_VM_SOURCE_ACTOR)
        ORES_VM_EXPORT_INIT(&blob->instance);
#endif
        blob->ready = 1;
    }
#if defined(ORES_VM_SOURCE_ACTOR)
    /* Compiled actors take and return one i64 today. */
    (void)self;
    int64_t message;
    if (input_length != sizeof message) return -2;
    if (capacity < sizeof message) return -3;
    memcpy(&message, cage + input, sizeof message);
    const int64_t reply = (int64_t)ORES_VM_EXPORT_ON_MESSAGE(&blob->instance, (uint64_t)message);
    memcpy(cage + output, &reply, sizeof reply);
    return sizeof reply;
#else
    /* The module speaks the turn ABI directly, with offsets relative to its own memory. */
    return (int64_t)w2c_guest_turn(&blob->instance, self, 0, input - state, input_length, output - state, capacity);
#endif
}

static const OresVmGuestKind kinds[] = {{turn, ORES_VM_MEMORY_BYTES, 0, ORES_VM_SCRATCH_BYTES, sizeof(Blob)}};
__attribute__((visibility("default"))) const OresVmGuestModule ores_vm_guest_module = {ORES_VM_GUEST_ABI_VERSION, 1, kinds};
