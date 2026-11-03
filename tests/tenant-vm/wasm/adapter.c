/* Runs a Wasm module lowered to native by wasm2c + clang as a tenant VM guest. Generated code
 * addresses its linear memory as base + u32 + u32 displacement, with no bounds checks. That
 * is the cage rule exactly, so the VM's cage serves directly as the linear memory. */
#include "oreslang/runtime/TenantVmGuestAbi.h"
#include "guest.h"
#include "guest_module.h"

#include <stdint.h>

static _Thread_local const OresVmHostApi* t_host;

/* What generated code needs from a Wasm runtime, reduced to this VM's behaviour. */
void wasm_rt_trap(wasm_rt_trap_t code) { (void)code; t_host->trap(); __builtin_unreachable(); }
bool wasm_rt_is_initialized(void) { return true; }
void wasm_rt_init(void) {}
_Thread_local uint32_t wasm_rt_call_stack_depth;
uint64_t wasm_rt_grow_memory(wasm_rt_memory_t* memory, uint64_t pages) { (void)memory; (void)pages; return (uint64_t)-1; }

/* No malloc on this path. A kill can land anywhere in guest mode, and one landing inside
 * malloc would leave the process-wide allocator lock held forever. */
enum { MAX_TABLE_ELEMENTS = 256 };
static _Thread_local wasm_rt_funcref_t t_table[MAX_TABLE_ELEMENTS];
void wasm_rt_allocate_funcref_table(wasm_rt_funcref_table_t* table, uint32_t elements, uint32_t max_elements) {
    if (elements > MAX_TABLE_ELEMENTS) t_host->trap();
    for (uint32_t i = 0; i < elements; ++i) t_table[i] = wasm_rt_funcref_null_value;
    table->data = t_table;
    table->size = elements;
    table->max_size = max_elements;
}
void wasm_rt_free_funcref_table(wasm_rt_funcref_table_t* table) { table->data = 0; table->size = 0; }

struct w2c_env { wasm_rt_memory_t* memory; };
wasm_rt_memory_t* w2c_env_memory(struct w2c_env* env) { return env->memory; }

static int64_t turn(const OresVmHostApi* host, uint8_t* cage, uint64_t self, uint32_t state, uint32_t input,
                    uint32_t input_length, uint32_t output, uint32_t capacity) {
    t_host = host;
    wasm_rt_memory_t memory = {0};
    memory.data = cage;
    memory.pages = 65536;
    memory.max_pages = 65536;
    memory.size = UINT64_C(1) << 32;
    struct w2c_env env = {&memory};
    /* Fresh per turn: Wasm globals live here, and turns of one VM can run in parallel. */
    w2c_guest instance;
    wasm2c_guest_instantiate(&instance, &env);
    const int64_t result = (int64_t)w2c_guest_turn(&instance, self, state, input, input_length, output, capacity);
    wasm2c_guest_free(&instance);
    return result;
}

static const OresVmGuestKind kinds[] = {{turn, 16, 0, 256}};
const OresVmGuestModule ores_vm_lowered_module = {ORES_VM_GUEST_ABI_VERSION, 1, kinds};
