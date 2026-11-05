#pragma once
/*
 * Guest ABI v5 for code running inside an in-process tenant VM.
 *
 * Graal isolates work because guest code is produced by a trusted compiler and can only
 * name memory relative to its isolate's heap base. This is the same contract:
 *
 *   Every guest load and store MUST address
 *       cage + (uint64_t)(uint32_t)base + (uint64_t)(uint32_t)offset + disp
 *   with disp < 4 GiB, and base one of the offsets the host passed this turn. Guest code holds
 *   offsets, never host pointers. It has no libc beyond memcpy-class leaf functions, no
 *   syscalls, no writable globals that survive a turn, and no inline assembly.
 *
 * Under that rule a guest cannot form an address outside its cage: the 4 GiB addressable
 * window is followed by more than 8 GiB of guard, so any base plus offset plus displacement
 * lands in its own cage or faults, and a fault ends only this VM.
 *
 * THE RULE IS NOT ENFORCED HERE. Code that breaks it can reach every tenant in the
 * process. Whatever produces guest code (compiler pass, or validated Wasm lowered to
 * native) is therefore the security boundary, as Graal's compiler is for its isolates.
 */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ORES_VM_GUEST_ABI_VERSION 5u

/* The only form a guest memory access may take. */
#define ORES_VM_AT(type, cage, offset) \
    ((type*)((cage) + (uint64_t)(uint32_t)(offset)))

/* Host calls. Offsets are validated by the host; a bad one returns failure. The caller
 * is implied by the current turn, so no authority token crosses this boundary. */
typedef struct {
    uint64_t target;
    int64_t value; /* delivered as these 8 bytes */
} OresVmSend;

typedef struct {
    uint32_t abi_version;
    /* Allocate from this actor's heap. 0 when its budget is spent. */
    uint32_t (*alloc)(uint32_t bytes, uint32_t alignment);
    /* Return a block to this actor's heap, with the size and alignment it was allocated with.
     * A wrong value can only confuse this actor's own heap. 1 if accepted. */
    int (*free)(uint32_t offset, uint32_t bytes, uint32_t alignment);
    /* Copy [offset, offset+length) to an actor in THIS VM. 1 if admitted. Other tenants'
     * actors cannot be named: a reference only resolves within the caller's VM. */
    int (*send)(uint64_t target, uint32_t offset, uint32_t length);
    /* New actor in this VM, owned by the caller: it is stopped after the caller ends. 0 when refused. */
    uint64_t (*spawn)(uint32_t kind);
    /* Its queued mail still runs, then it ends. Also accepts this actor's own reference. 1 if accepted. */
    int (*stop)(uint64_t target);
    /* Makes a turn's effects happen together. Admits all of these sends or none, and if it admits
     * them adds 1 to the version word: the first 4 bytes of stored state, which must already hold what
     * that version means. Nothing can stop the turn after it returns 1, so a kill cannot report
     * failure for a turn whose effects stayed. It must be the last thing a turn does, because
     * after it faults are no longer contained. 0 means nothing happened, and the turn goes on. */
    int (*commit)(const OresVmSend* sends, uint32_t count);
    /* Ends this VM. Does not return. For checks in lowered code (Wasm `unreachable`). */
    void (*trap)(void);
} OresVmHostApi;

/* Returns the output length written at `output`, or a negative typed guest error.
 * State and heap persist across turns. Input and output are valid for this turn only.
 * `instance` is `instance_bytes` of zeroed memory private to this actor and outside the cage,
 * so guest load and store instructions cannot name it. Lowered code keeps what must not be
 * guest-writable there, such as Wasm globals and tables. */
typedef int64_t (*OresVmGuestTurn)(const OresVmHostApi* host, void* instance, uint8_t* cage, uint64_t self,
                                   uint32_t state, uint32_t input, uint32_t input_length,
                                   uint32_t output, uint32_t output_capacity);

/* A fault in a turn of this kind cannot have touched memory outside this actor's region, so the
 * actor alone can be restarted. Only lowering that proves it may set this: bounds-checked Wasm can,
 * and code that merely follows the addressing rule cannot, since it could write over a sibling. */
#define ORES_VM_KIND_RESTARTABLE 1u

typedef struct {
    OresVmGuestTurn turn;
    uint32_t state_bytes;
    uint32_t heap_bytes;
    uint32_t scratch_bytes; /* holds this turn's input and output */
    uint32_t instance_bytes;
    /* What survives a restart: this many bytes right after `instance`, also outside the cage. A
     * restart clears the actor's region and instance, and the guest loads its state back from here.
     * A kill can land anywhere, so the guest must commit to it in a way a kill cannot tear: see commit.
     * The first 4 bytes are a version, 0 until anything is committed. */
    uint32_t stored_bytes;
    uint32_t flags;
} OresVmGuestKind;

typedef struct {
    uint32_t abi_version;
    uint32_t kind_count;
    const OresVmGuestKind* kinds;
} OresVmGuestModule;

/* A loadable guest library exports this one symbol. */
#define ORES_VM_GUEST_MODULE_SYMBOL "ores_vm_guest_module"

#ifdef __cplusplus
}
#endif
