#pragma once
/*
 * Guest ABI v1 for code running inside an in-process tenant VM.
 *
 * Graal isolates work because guest code is produced by a trusted compiler and can only
 * name memory relative to its isolate's heap base. This is the same contract:
 *
 *   Every guest load and store MUST address `cage + (uint64_t)(uint32_t)offset + disp`,
 *   with disp < 4 GiB. Guest code holds offsets, never host pointers. It has no libc, no
 *   syscalls, no writable globals and no inline assembly.
 *
 * Under that rule, a guest cannot form an address outside its cage: the 4 GiB addressable
 * window is followed by a 4 GiB guard, so any offset plus displacement lands in its own
 * cage or faults, and a fault ends only this VM.
 *
 * THE RULE IS NOT ENFORCED HERE. Code that breaks it can reach every tenant in the
 * process. Whatever produces guest code (compiler pass, or validated Wasm lowered to
 * native) is therefore the security boundary, as Graal's compiler is for its isolates.
 */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ORES_VM_GUEST_ABI_VERSION 1u

/* The only form a guest memory access may take. */
#define ORES_VM_AT(type, cage, offset) \
    ((type*)((cage) + (uint64_t)(uint32_t)(offset)))

/* Host calls. Offsets are validated by the host; a bad one returns failure. The caller
 * is implied by the current turn, so no authority token crosses this boundary. */
typedef struct {
    uint32_t abi_version;
    /* Bump allocate from this actor's heap. 0 when its budget is spent. */
    uint32_t (*alloc)(uint32_t bytes, uint32_t alignment);
    /* Copy [offset, offset+length) to an actor in THIS VM. 1 if admitted. Other tenants'
     * actors cannot be named: a reference only resolves within the caller's VM. */
    int (*send)(uint64_t target, uint32_t offset, uint32_t length);
    /* New actor in this VM. 0 when refused. */
    uint64_t (*spawn)(uint32_t kind);
    /* Ends this VM. Does not return. For checks in lowered code (Wasm `unreachable`). */
    void (*trap)(void);
} OresVmHostApi;

/* Returns the output length written at `output`, or a negative typed guest error.
 * State and heap persist across turns. Input and output are valid for this turn only. */
typedef int64_t (*OresVmGuestTurn)(const OresVmHostApi* host, uint8_t* cage, uint64_t self,
                                   uint32_t state, uint32_t input, uint32_t input_length,
                                   uint32_t output, uint32_t output_capacity);

typedef struct {
    OresVmGuestTurn turn;
    uint32_t state_bytes;
    uint32_t heap_bytes;
    uint32_t scratch_bytes; /* holds this turn's input and output */
} OresVmGuestKind;

typedef struct {
    uint32_t abi_version;
    uint32_t kind_count;
    const OresVmGuestKind* kinds;
} OresVmGuestModule;

#ifdef __cplusplus
}
#endif
