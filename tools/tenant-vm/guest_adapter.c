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

struct w2c_env {
    wasm_rt_memory_t* memory;
    uint8_t* send_buffer;  /* the last 8 bytes of this actor's scratch */
    uint32_t send_offset;
};

typedef struct {
    w2c_guest instance;
    wasm_rt_memory_t memory;
    struct w2c_env env;
#if ORES_VM_TABLE_SLOTS
    wasm_rt_funcref_t table[ORES_VM_TABLE_SLOTS];
#endif
    uint32_t ready;
    uint32_t main_ran;
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

/* Source actor operations. A refusal ends this VM: a source program has no way to see one, and
 * carrying on would turn a lost message into a wrong answer that looks fine. */
uint64_t w2c_env_oreslang_vm_spawn(struct w2c_env* env, uint64_t kind) {
    (void)env;
    const uint64_t reference = kind > UINT32_MAX ? 0 : __atomic_load_n(&g_host, __ATOMIC_RELAXED)->spawn((uint32_t)kind);
    if (!reference) wasm_rt_trap(WASM_RT_TRAP_OOB);
    return reference;
}
void w2c_env_oreslang_vm_send(struct w2c_env* env, uint64_t target, uint64_t message) {
    memcpy(env->send_buffer, &message, sizeof message);
    if (!__atomic_load_n(&g_host, __ATOMIC_RELAXED)->send(target, env->send_offset, sizeof message)) wasm_rt_trap(WASM_RT_TRAP_OOB);
}
void w2c_env_oreslang_vm_stop(struct w2c_env* env, uint64_t target) {
    (void)env;
    if (!__atomic_load_n(&g_host, __ATOMIC_RELAXED)->stop(target)) wasm_rt_trap(WASM_RT_TRAP_OOB);
}

/* First turn of an actor: its memory is its region of the cage, and its instance is made once. */
static Blob* enter(const OresVmHostApi* host, void* raw, uint8_t* cage, uint32_t state) {
    if (__atomic_load_n(&g_host, __ATOMIC_RELAXED) != host) __atomic_store_n(&g_host, host, __ATOMIC_RELAXED);
    Blob* blob = (Blob*)raw;
    if (!blob->ready) {
        blob->memory.data = cage + state;
        blob->memory.pages = ORES_VM_MEMORY_BYTES / 65536;
        blob->memory.max_pages = ORES_VM_MEMORY_BYTES / 65536;
        /* The whole region: linear memory, then this turn's scratch. */
        blob->memory.size = ORES_VM_MEMORY_BYTES + ORES_VM_SCRATCH_BYTES;
        blob->env.memory = &blob->memory;
        blob->env.send_offset = state + ORES_VM_MEMORY_BYTES + ORES_VM_SCRATCH_BYTES - 8;
        blob->env.send_buffer = cage + blob->env.send_offset;
        wasm2c_guest_instantiate(&blob->instance, &blob->env);
    }
    return blob;
}

#if defined(ORES_VM_SOURCE_ACTOR)
/* What a restarted actor gets back: its fields as of its last turn that finished. A kill can land
 * anywhere, so a turn writes the slot not in use, then flips `version` with one store. Whatever
 * happens, the slot it names is complete. Version 0 means nothing is committed yet. */
enum { STORED_FIELDS = 16 };
typedef struct {
    uint32_t version;
    uint32_t reserved;
    int64_t slots[2][STORED_FIELDS];
} Stored;

/* Only bounds checks show that a fault in this actor touched nothing outside it. */
#if defined(WASM_RT_MEMCHECK_BOUNDS_CHECK) && WASM_RT_MEMCHECK_BOUNDS_CHECK
#define ORES_VM_ACTOR_FLAGS ORES_VM_KIND_RESTARTABLE
#else
#define ORES_VM_ACTOR_FLAGS 0
#endif

/* Only in test builds, to land a fault exactly in a commit, and to show the test can tell: the unsafe
 * variant writes over the slot in use, which is the bug the flip exists to prevent. */
#if defined(ORES_VM_TEST_COMMIT_FAULT_ON)
#define ORES_VM_COMMIT_FAULT_POINT(message, i) \
    do { if ((message) == ORES_VM_TEST_COMMIT_FAULT_ON && (i) == 0) wasm_rt_trap(WASM_RT_TRAP_OOB); } while (0)
#else
#define ORES_VM_COMMIT_FAULT_POINT(message, i) do {} while (0)
#endif
#if defined(ORES_VM_TEST_UNSAFE_COMMIT)
#define ORES_VM_COMMIT_SLOT(version) 1u
#else
#define ORES_VM_COMMIT_SLOT(version) (version)
#endif

/* Compiled actors take and return one i64 today. kinds.inc lists one line per actor type. */
#define ORES_VM_ACTOR(index, init, on_message, fields, field_get, field_set)                                          \
    static int64_t actor_turn_##index(const OresVmHostApi* host, void* raw, uint8_t* cage, uint64_t self, uint32_t state, \
                                      uint32_t input, uint32_t input_length, uint32_t output, uint32_t capacity) {      \
        (void)self;                                                                                                   \
        Blob* blob = enter(host, raw, cage, state);                                                                   \
        Stored* stored = (Stored*)((char*)raw + sizeof(Blob));                                                        \
        const uint32_t count = fields(&blob->instance);                                                               \
        if (count > STORED_FIELDS) wasm_rt_trap(WASM_RT_TRAP_OOB);                                                    \
        if (!blob->ready) {                                                                                           \
            init(&blob->instance);                                                                                    \
            const uint32_t version = __atomic_load_n(&stored->version, __ATOMIC_ACQUIRE);                             \
            if (version) {                                                                                            \
                for (uint32_t i = 0; i < count; ++i)                                                                  \
                    field_set(&blob->instance, i, (uint64_t)stored->slots[version & 1][i]);                           \
            }                                                                                                         \
            blob->ready = 1;                                                                                          \
        }                                                                                                             \
        int64_t message;                                                                                              \
        if (input_length != sizeof message) return -2;                                                                \
        if (capacity < sizeof message) return -3;                                                                     \
        memcpy(&message, cage + input, sizeof message);                                                               \
        const int64_t reply = (int64_t)on_message(&blob->instance, (uint64_t)message);                                \
        const uint32_t next = __atomic_load_n(&stored->version, __ATOMIC_RELAXED) + 1;                                \
        const uint32_t committed = ORES_VM_COMMIT_SLOT(next ? next : 2);                                              \
        for (uint32_t i = 0; i < count; ++i) {                                                                        \
            stored->slots[committed & 1][i] = (int64_t)field_get(&blob->instance, i);                                 \
            ORES_VM_COMMIT_FAULT_POINT(message, i);                                                                   \
        }                                                                                                             \
        __atomic_store_n(&stored->version, committed, __ATOMIC_RELEASE);                                              \
        memcpy(cage + output, &reply, sizeof reply);                                                                  \
        return sizeof reply;                                                                                          \
    }
/* The program's main: runs once, in a turn of its own kind, and replies with its result. It is not
 * restartable: it may already have spawned and sent, and running it again would do so twice. */
#define ORES_VM_MAIN(index, entry)                                                                                    \
    static int64_t main_turn(const OresVmHostApi* host, void* raw, uint8_t* cage, uint64_t self, uint32_t state,       \
                             uint32_t input, uint32_t input_length, uint32_t output, uint32_t capacity) {               \
        (void)self; (void)input; (void)input_length;                                                                  \
        Blob* blob = enter(host, raw, cage, state);                                                                   \
        blob->ready = 1;                                                                                              \
        if (blob->main_ran) return -4;                                                                                \
        if (capacity < sizeof(int64_t)) return -3;                                                                    \
        blob->main_ran = 1;                                                                                           \
        const int64_t reply = (int64_t)entry(&blob->instance);                                                        \
        memcpy(cage + output, &reply, sizeof reply);                                                                  \
        return sizeof reply;                                                                                          \
    }
#include "kinds.inc"
#undef ORES_VM_ACTOR
#undef ORES_VM_MAIN
#define ORES_VM_ACTOR(index, init, on_message, fields, field_get, field_set) \
    {actor_turn_##index, ORES_VM_MEMORY_BYTES, 0, ORES_VM_SCRATCH_BYTES, sizeof(Blob), sizeof(Stored), ORES_VM_ACTOR_FLAGS},
#define ORES_VM_MAIN(index, entry) {main_turn, ORES_VM_MEMORY_BYTES, 0, ORES_VM_SCRATCH_BYTES, sizeof(Blob), 0, 0},
static const OresVmGuestKind kinds[] = {
#include "kinds.inc"
};
#else
/* The module speaks the turn ABI directly, with offsets relative to its own memory. */
static int64_t turn(const OresVmHostApi* host, void* raw, uint8_t* cage, uint64_t self, uint32_t state, uint32_t input,
                    uint32_t input_length, uint32_t output, uint32_t capacity) {
    Blob* blob = enter(host, raw, cage, state);
    blob->ready = 1;
    return (int64_t)w2c_guest_turn(&blob->instance, self, 0, input - state, input_length, output - state, capacity);
}
static const OresVmGuestKind kinds[] = {{turn, ORES_VM_MEMORY_BYTES, 0, ORES_VM_SCRATCH_BYTES, sizeof(Blob), 0, 0}};
#endif

__attribute__((visibility("default"))) const OresVmGuestModule ores_vm_guest_module = {
    ORES_VM_GUEST_ABI_VERSION, sizeof kinds / sizeof kinds[0], kinds};
