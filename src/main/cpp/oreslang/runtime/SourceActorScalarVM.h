#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/*
 * Compiler-owned, source-actor-only pure i64 call graph v1. A program is
 * generated from a closed, prevalidated same-module function graph. No actor
 * field, pointer, closure, FFI, async, borrow or effect is accepted here.
 * Program descriptions are readonly constants emitted into the native image.
 */
typedef struct {
    uint64_t opcode;
    int64_t immediate;
} OresActorScalarInstruction;

typedef struct {
    uint64_t function_count;
    uint64_t instruction_count;
    uint64_t root_function;
    const uint64_t* function_entries;
    const OresActorScalarInstruction* instructions;
} OresActorScalarProgram;

enum OresActorScalarOpcode {
    ORES_SCALAR_PUSH_I64 = 1,
    ORES_SCALAR_LOAD_ARG = 2,
    ORES_SCALAR_LOAD_LOCAL = 3,
    ORES_SCALAR_STORE_LOCAL = 4,
    ORES_SCALAR_ADD = 5,
    ORES_SCALAR_SUB = 6,
    ORES_SCALAR_MUL = 7,
    ORES_SCALAR_EQ = 8,
    ORES_SCALAR_NE = 9,
    ORES_SCALAR_LT = 10,
    ORES_SCALAR_LE = 11,
    ORES_SCALAR_GT = 12,
    ORES_SCALAR_GE = 13,
    ORES_SCALAR_NOT = 14,
    ORES_SCALAR_BRANCH_FALSE = 15,
    ORES_SCALAR_CALL = 16,
    ORES_SCALAR_TAIL_CALL = 17,
    ORES_SCALAR_RETURN = 18
};

enum OresActorScalarStatus {
    ORES_SCALAR_OK = 0,
    ORES_SCALAR_FRAME_QUOTA = 1,
    ORES_SCALAR_FUEL_EXHAUSTED = 2,
    ORES_SCALAR_INVALID_PROGRAM = 3,
    ORES_SCALAR_INVALID_FRAME = 4,
    ORES_SCALAR_CANCELLED = 5
};

enum {
    ORES_SCALAR_MAX_FUNCTIONS = 16,
    ORES_SCALAR_MAX_INSTRUCTIONS = 4096,
    ORES_SCALAR_MAX_FRAMES = 64,
    ORES_SCALAR_MAX_LOCAL_SLOTS = 4,
    ORES_SCALAR_MAX_OPERAND_SLOTS = 16
};

/* The caller is compiler-generated code inside an admitted physical native
 * source actor callback. The runtime stores real PC, function, parameter,
 * local and tagged operand cells in the actor's frame_words, never a native
 * recursive C call chain. Returns status; on failure the host records actor-
 * local failure and seals this actor's mailbox after the callback exits.
 * result is zeroed on any failure and no frame pointer is returned.
 */
int oreslang_source_actor_scalar_vm_execute(
    const OresActorScalarProgram* program, int64_t argument, int64_t* result);

#ifdef __cplusplus
}
#endif
