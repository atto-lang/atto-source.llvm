#include "oreslang/runtime/SourceActorBootstrap.h"
#include "oreslang/runtime/SourceActorScalarVM.h"
#include "oreslang/runtime/NativeOresVM.h"
#include "oreslang/runtime/NativeAwaitTraceBridge.h"
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SOURCE_ACTOR_MAGIC UINT64_C(0x4f52455353504157)
#define SOURCE_GROUP_SLOTS 64u
#define SOURCE_GROUP_ROOT_SLOT (SOURCE_GROUP_SLOTS - 1u)
#define SOURCE_GROUP_CARRIERS 4u
#define SOURCE_GROUP_MAILBOX 128u
#define SOURCE_TRACE_EXECUTIONS 128u
#define SOURCE_TRACE_CONTINUATIONS 128u
#define SOURCE_ACTOR_FRAME_WORDS 8192u /* 64 KiB, actor-incarnation owned */
#define SOURCE_SCALAR_FRAME_BASE 32u /* reserve M2 handler scalar slots */
#define SOURCE_SCALAR_FRAME_WORDS 48u /* header + 4 typed locals + 16 operands */
#define SOURCE_SCALAR_FRAME_MAGIC UINT64_C(0x4154544f46524d33)
#define SOURCE_SCALAR_TAG_I64 UINT64_C(1)
#define SOURCE_SCALAR_TAG_BOOL UINT64_C(2)
#define SOURCE_SCALAR_MAX_FUEL UINT64_C(2000000)
#define SOURCE_SCALAR_CHECK_INTERVAL UINT64_C(256)

/* Machine/layout invariants are compile-time, not runtime guesses. The
 * shared first 32 words belong solely to the M2 scalar handler places.
 * A typed VM frame cannot overlap them or spill outside the actor's heap. */
_Static_assert(sizeof(OresActorScalarInstruction) == 16,
    "LLVM/native scalar instruction ABI must be two i64 words");
_Static_assert(offsetof(OresActorScalarProgram, function_entries)
        == 3 * sizeof(uint64_t),
    "LLVM/native scalar program ABI metadata layout mismatch");
_Static_assert(SOURCE_SCALAR_FRAME_BASE
        + ORES_SCALAR_MAX_FRAMES * SOURCE_SCALAR_FRAME_WORDS
        <= SOURCE_ACTOR_FRAME_WORDS,
    "bounded scalar frames exceed source actor incarnation quota");
_Static_assert(1u + 16u + 8u + 1u <= SOURCE_SCALAR_FRAME_BASE,
    "M2 scalar argument, cached fields and temporary local slots overlap VM frames");
#define SOURCE_ROOT_CONTROL_SUSPEND UINT64_C(1)
#define SOURCE_ROOT_CONTROL_RESUME UINT64_C(2)
#define SOURCE_ROOT_CONTROL_DONE_SUSPEND UINT64_C(3)
#define SOURCE_ROOT_CONTROL_DONE_RESUME UINT64_C(4)

typedef struct {
    uint64_t magic;
    uint64_t id;
    OresActorI64Heap* heap;
    uint64_t* frame_words; /* actor-incarnation-owned, not carrier-stack memory */
    atomic_int frame_active; /* admits at most one source callback */
    atomic_int frame_vm_error; /* sticky, per-actor fail-stop reason */
    atomic_int frame_vm_active; /* forbids nested VM frame overwrite */
    atomic_uint_fast64_t frame_vm_checkpoints;
    atomic_uint_fast64_t frame_vm_steps;
    atomic_uint_fast64_t frame_vm_tail_calls;
    atomic_uint_fast64_t frame_vm_non_tail_calls;
    uint64_t frame_domain;
    uint64_t frame_epoch;
    uint64_t frame_slot_calls;
    uint64_t frame_high_water;
    OresNativeActorRef ref;
    OresSourceActorInit constructor;
    OresSourceActorMessage on_message;
    atomic_int initialized;
    atomic_int startup_state;
    atomic_int constructor_started;
    atomic_int stop_requested;
    atomic_int join_started;
    atomic_uint_fast64_t processed;
    atomic_int_fast64_t last_result;
} SourceActor;

typedef struct {
    pthread_cond_t completed;
    OresNativeActorFuture* ready_future;
    OresSourceActorContinuation continuation;
    SourceActor* actor;
    OresNativeSuspensionLink link;
    uint64_t source_token;
    int suspend_recorded;
    int done;
    int outcome;
    int64_t result;
} ReadyAwaitFrame;

typedef struct {
    pthread_cond_t completed;
    OresNativeActorFuture* done_future;
    OresSourceActorContinuation continuation;
    SourceActor* actor;
    OresNativeSuspensionLink link;
    uint64_t source_token;
    int suspend_recorded;
    int done;
    int64_t result;
} DoneAwaitFrame;

typedef struct {
    pthread_mutex_t mutex;
    OresNativeVM* vm;
    OresNativeActorRef root_ref;
    OresNativeExecutionContextStore* executions;
    OresNativeContinuationStore* continuations;
    ReadyAwaitFrame* pending_ready;
    DoneAwaitFrame* pending_done;
    SourceActor* actors[SOURCE_GROUP_SLOTS];
    size_t next_slot;
    size_t live;
    size_t peak;
} SourceActorGroup;

/* Process-local trusted-main group; the compiler only supports linear refs
 * in a zero-argument top-level main. There is exactly one worker pool per
 * active group, not one pool per actor.
 */
static SourceActorGroup group = { .mutex = PTHREAD_MUTEX_INITIALIZER };
static _Atomic uint64_t next_actor_id = 4100;

/* Source ActorRef incarnation IDs never wrap/reissue earlier authority. */
static uint64_t reserve_source_actor_id(void) {
    uint64_t next = atomic_load_explicit(
        &next_actor_id, memory_order_relaxed);
    while (next != 0 && next != UINT64_MAX) {
        if (atomic_compare_exchange_weak_explicit(
                &next_actor_id, &next, next + 1,
                memory_order_relaxed, memory_order_relaxed))
            return next;
    }
    return 0;
}
static _Thread_local OresNativeExecutionHandle current_source_execution;
static _Thread_local SourceActor* current_source_frame_actor;
static void ensure(int condition);
#ifdef ORESLANG_SOURCE_ACTOR_TESTING
static atomic_int test_hold_scalar_checkpoint;
static atomic_int test_scalar_checkpoint_entered;
void oreslang_source_actor_test_hold_scalar_checkpoint(int hold) {
    if (hold)
        atomic_store_explicit(&test_scalar_checkpoint_entered, 0,
            memory_order_release);
    atomic_store_explicit(&test_hold_scalar_checkpoint, hold != 0,
        memory_order_release);
}
int oreslang_source_actor_test_scalar_checkpoint_entered(void) {
    return atomic_load_explicit(&test_scalar_checkpoint_entered,
        memory_order_acquire);
}
uint64_t oreslang_source_actor_test_scalar_checkpoint_count(void* raw) {
    SourceActor* actor = (SourceActor*)raw;
    if (!actor || actor->magic != SOURCE_ACTOR_MAGIC)
        return UINT64_MAX;
    return atomic_load_explicit(&actor->frame_vm_checkpoints,
        memory_order_acquire);
}
#endif

/*
 * The C ABI for compiler-owned scalar actor frame places. The native heap
 * grants the physical actor turn; this per-incarnation byte store owns its
 * separate logical scalar slots. It is intentionally not an OS stack or an
 * arbitrary pointer-escape boundary: only closed scalar compiler lowering
 * may obtain a slot pointer while the originating actor turn is live.
 */
static void source_frame_enter(SourceActor* actor, OresActorTurn turn) {
    ensure(actor && actor->magic == SOURCE_ACTOR_MAGIC
        && actor->frame_words != NULL && turn.domain && turn.epoch
        && current_source_frame_actor == NULL);
    int expected = 0;
    ensure(atomic_compare_exchange_strong_explicit(
        &actor->frame_active, &expected, 1,
        memory_order_acq_rel, memory_order_acquire));
    actor->frame_domain = turn.domain;
    actor->frame_epoch = turn.epoch;
    current_source_frame_actor = actor;
}

int oreslang_source_actor_frame_i64_slot_checked(
        uint64_t domain, uint64_t epoch, uint64_t index, int64_t** out) {
    if (!out) return 0;
    *out = NULL;
    SourceActor* actor = current_source_frame_actor;
    if (!actor || actor->magic != SOURCE_ACTOR_MAGIC
        || !actor->frame_words || !domain || !epoch
        || actor->frame_domain != domain || actor->frame_epoch != epoch
        || index >= SOURCE_ACTOR_FRAME_WORDS
        || atomic_load_explicit(&actor->frame_active, memory_order_acquire) != 1)
        return 0;
    if (actor->frame_slot_calls == UINT64_MAX)
        return 0; /* terminal accounting overflow, no silent wrap */
    ++actor->frame_slot_calls;
    if (actor->frame_high_water < index + 1)
        actor->frame_high_water = index + 1;
    *out = (int64_t*)(actor->frame_words + index);
    return 1;
}

int64_t* oreslang_source_actor_frame_i64_slot(
        uint64_t domain, uint64_t epoch, uint64_t index) {
    int64_t* result = NULL;
    ensure(oreslang_source_actor_frame_i64_slot_checked(
        domain, epoch, index, &result));
    return result;
}

static void source_frame_leave(SourceActor* actor, OresActorTurn turn) {
    ensure(actor && current_source_frame_actor == actor
        && actor->frame_domain == turn.domain
        && actor->frame_epoch == turn.epoch
        && atomic_load_explicit(&actor->frame_active, memory_order_acquire) == 1);
    /* No compiler-generated pointer into this storage may survive the turn.
     * Use volatile stores to prevent the terminal wipe being optimized away. */
    volatile uint64_t* bytes = (volatile uint64_t*)actor->frame_words;
    for (size_t i = 0; i < SOURCE_ACTOR_FRAME_WORDS; ++i)
        bytes[i] = 0;
    actor->frame_domain = 0;
    actor->frame_epoch = 0;
    current_source_frame_actor = NULL;
    atomic_store_explicit(&actor->frame_active, 0, memory_order_release);
}


/*
 * M3 source-compiled unary scalar frame machine. All frame state lives in
 * actor->frame_words, 64 bounded frames x 48 words, not a C recursive call
 * chain. Every frame contains typed PC, function, parameter, locals, operand
 * slots and an actor-generation/turn lease. Metadata/code are readonly
 * compiler-owned program constants. A malformed program fails closed before
 * executing an instruction; dynamically malformed frames also fail closed.
 */
static uint64_t scalar_bits(int64_t value) {
    uint64_t raw;
    memcpy(&raw, &value, sizeof(raw));
    return raw;
}
static int64_t scalar_signed(uint64_t bits) {
    int64_t value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}
static uint64_t* scalar_frame(SourceActor* actor, size_t depth) {
    return actor->frame_words + SOURCE_SCALAR_FRAME_BASE
        + depth * SOURCE_SCALAR_FRAME_WORDS;
}
static void scalar_clear_frame(SourceActor* actor, size_t depth) {
    volatile uint64_t* words = (volatile uint64_t*)scalar_frame(actor, depth);
    for (size_t i = 0; i < SOURCE_SCALAR_FRAME_WORDS; ++i) words[i] = 0;
}
static void scalar_init_frame(
        SourceActor* actor, size_t depth, uint64_t function,
        uint64_t pc, uint64_t epoch, int64_t argument) {
    uint64_t* f = scalar_frame(actor, depth);
    f[0] = SOURCE_SCALAR_FRAME_MAGIC;
    f[1] = actor->id;
    f[2] = epoch;
    f[3] = function;
    f[4] = pc;
    f[5] = 0;
    f[6] = scalar_bits(argument);
    f[7] = SOURCE_SCALAR_TAG_I64;
}
static int scalar_push(uint64_t* frame, uint64_t tag, uint64_t bits) {
    const uint64_t sp = frame[5];
    if (sp >= ORES_SCALAR_MAX_OPERAND_SLOTS) return 0;
    frame[16 + 2 * sp] = tag;
    frame[17 + 2 * sp] = bits;
    frame[5] = sp + 1;
    return 1;
}
static int scalar_pop(
        uint64_t* frame, uint64_t expected, uint64_t* result) {
    const uint64_t sp = frame[5];
    if (!sp || sp > ORES_SCALAR_MAX_OPERAND_SLOTS) return 0;
    const size_t pos = 16 + 2 * (size_t)(sp - 1);
    if (frame[pos] != expected) return 0;
    *result = frame[pos + 1];
    frame[pos] = 0;
    frame[pos + 1] = 0;
    frame[5] = sp - 1;
    return 1;
}
static int scalar_preflight(const OresActorScalarProgram* program) {
    if (!program || !program->function_entries || !program->instructions
        || !program->function_count
        || program->function_count > ORES_SCALAR_MAX_FUNCTIONS
        || !program->instruction_count
        || program->instruction_count > ORES_SCALAR_MAX_INSTRUCTIONS
        || program->root_function >= program->function_count
        || program->function_entries[0] != 0)
        return 0;
    for (uint64_t function = 0; function < program->function_count; ++function) {
        const uint64_t begin = program->function_entries[function];
        const uint64_t end = function + 1 < program->function_count
            ? program->function_entries[function + 1]
            : program->instruction_count;
        if (begin >= end || end > program->instruction_count) return 0;
        const uint64_t last = program->instructions[end - 1].opcode;
        if (last != ORES_SCALAR_RETURN
                && last != ORES_SCALAR_TAIL_CALL)
            return 0;
        for (uint64_t i = begin; i < end; ++i) {
            const OresActorScalarInstruction ins = program->instructions[i];
            switch (ins.opcode) {
                case ORES_SCALAR_PUSH_I64:
                    break;
                case ORES_SCALAR_LOAD_ARG:
                case ORES_SCALAR_ADD:
                case ORES_SCALAR_SUB:
                case ORES_SCALAR_MUL:
                case ORES_SCALAR_EQ:
                case ORES_SCALAR_NE:
                case ORES_SCALAR_LT:
                case ORES_SCALAR_LE:
                case ORES_SCALAR_GT:
                case ORES_SCALAR_GE:
                case ORES_SCALAR_NOT:
                case ORES_SCALAR_RETURN:
                    if (ins.immediate) return 0;
                    break;
                case ORES_SCALAR_LOAD_LOCAL:
                case ORES_SCALAR_STORE_LOCAL:
                    if (ins.immediate < 0
                            || ins.immediate >= ORES_SCALAR_MAX_LOCAL_SLOTS)
                        return 0;
                    break;
                case ORES_SCALAR_BRANCH_FALSE:
                    if (ins.immediate < 0
                            || (uint64_t)ins.immediate < begin
                            || (uint64_t)ins.immediate >= end)
                        return 0;
                    break;
                case ORES_SCALAR_CALL:
                case ORES_SCALAR_TAIL_CALL:
                    if (ins.immediate < 0
                            || (uint64_t)ins.immediate >= program->function_count)
                        return 0;
                    break;
                default:
                    return 0;
            }
        }
    }
    return 1;
}

int oreslang_source_actor_scalar_vm_execute(
        const OresActorScalarProgram* program,
        int64_t argument, int64_t* result) {
    if (!result) return ORES_SCALAR_INVALID_FRAME;
    *result = 0;
    SourceActor* actor = current_source_frame_actor;
    if (!actor || actor->magic != SOURCE_ACTOR_MAGIC
            || !actor->frame_words || !actor->frame_domain
            || !actor->frame_epoch
            || atomic_load_explicit(&actor->frame_active,
                memory_order_acquire) != 1)
        return ORES_SCALAR_INVALID_FRAME;
    if (atomic_load_explicit(&actor->frame_vm_error,
            memory_order_acquire) != ORES_SCALAR_OK)
        return ORES_SCALAR_INVALID_FRAME;
    int expected_vm_active = 0;
    if (!atomic_compare_exchange_strong_explicit(
            &actor->frame_vm_active, &expected_vm_active, 1,
            memory_order_acq_rel, memory_order_acquire)) {
        /* An accidental nested interpreter call must not overwrite a live
         * parent PC/locals frame. Poison this actor, not the carrier. */
        atomic_store_explicit(&actor->frame_vm_error,
            ORES_SCALAR_INVALID_FRAME, memory_order_release);
        return ORES_SCALAR_INVALID_FRAME;
    }
    int status = ORES_SCALAR_INVALID_PROGRAM;
    if (!scalar_preflight(program)) {
        atomic_store_explicit(&actor->frame_vm_error,
            status, memory_order_release);
        atomic_store_explicit(&actor->frame_vm_active, 0,
            memory_order_release);
        return status;
    }
    const uint64_t epoch = actor->frame_epoch;
    size_t depth = 1;
    uint64_t executed = 0;
    uint64_t tails = 0;
    uint64_t calls = 0;
    scalar_init_frame(actor, 0, program->root_function,
        program->function_entries[program->root_function],
        epoch, argument);
    status = ORES_SCALAR_INVALID_FRAME;
    while (executed < SOURCE_SCALAR_MAX_FUEL) {
        uint64_t* frame = scalar_frame(actor, depth - 1);
        if (frame[0] != SOURCE_SCALAR_FRAME_MAGIC
                || frame[1] != actor->id || frame[2] != epoch
                || frame[7] != SOURCE_SCALAR_TAG_I64
                || frame[3] >= program->function_count
                || frame[5] > ORES_SCALAR_MAX_OPERAND_SLOTS)
            goto cleanup;
        const uint64_t function = frame[3];
        const uint64_t first = program->function_entries[function];
        const uint64_t limit = function + 1 < program->function_count
            ? program->function_entries[function + 1]
            : program->instruction_count;
        const uint64_t pc = frame[4];
        if (pc < first || pc >= limit) goto cleanup;
        const OresActorScalarInstruction ins = program->instructions[pc];
        uint64_t lhs = 0, rhs = 0;
        ++executed;
        frame[4] = pc + 1;
        switch (ins.opcode) {
            case ORES_SCALAR_PUSH_I64:
                if (!scalar_push(frame, SOURCE_SCALAR_TAG_I64,
                        scalar_bits(ins.immediate))) goto cleanup;
                break;
            case ORES_SCALAR_LOAD_ARG:
                if (!scalar_push(frame, SOURCE_SCALAR_TAG_I64, frame[6]))
                    goto cleanup;
                break;
            case ORES_SCALAR_LOAD_LOCAL: {
                const size_t position = 8 + 2 * (size_t)ins.immediate;
                if (frame[position] != SOURCE_SCALAR_TAG_I64
                    || !scalar_push(frame, SOURCE_SCALAR_TAG_I64,
                                    frame[position + 1])) goto cleanup;
                break;
            }
            case ORES_SCALAR_STORE_LOCAL: {
                const size_t position = 8 + 2 * (size_t)ins.immediate;
                if (frame[position] || !scalar_pop(frame,
                        SOURCE_SCALAR_TAG_I64, &rhs)) goto cleanup;
                frame[position] = SOURCE_SCALAR_TAG_I64;
                frame[position + 1] = rhs;
                break;
            }
            case ORES_SCALAR_ADD:
            case ORES_SCALAR_SUB:
            case ORES_SCALAR_MUL:
            case ORES_SCALAR_EQ:
            case ORES_SCALAR_NE:
            case ORES_SCALAR_LT:
            case ORES_SCALAR_LE:
            case ORES_SCALAR_GT:
            case ORES_SCALAR_GE: {
                if (!scalar_pop(frame, SOURCE_SCALAR_TAG_I64, &rhs)
                        || !scalar_pop(frame, SOURCE_SCALAR_TAG_I64, &lhs))
                    goto cleanup;
                uint64_t value = 0;
                uint64_t tag = SOURCE_SCALAR_TAG_I64;
                switch (ins.opcode) {
                    case ORES_SCALAR_ADD: value = lhs + rhs; break;
                    case ORES_SCALAR_SUB: value = lhs - rhs; break;
                    case ORES_SCALAR_MUL: value = lhs * rhs; break;
                    case ORES_SCALAR_EQ: value = (lhs == rhs); tag = SOURCE_SCALAR_TAG_BOOL; break;
                    case ORES_SCALAR_NE: value = (lhs != rhs); tag = SOURCE_SCALAR_TAG_BOOL; break;
                    case ORES_SCALAR_LT: value = (scalar_signed(lhs) < scalar_signed(rhs)); tag = SOURCE_SCALAR_TAG_BOOL; break;
                    case ORES_SCALAR_LE: value = (scalar_signed(lhs) <= scalar_signed(rhs)); tag = SOURCE_SCALAR_TAG_BOOL; break;
                    case ORES_SCALAR_GT: value = (scalar_signed(lhs) > scalar_signed(rhs)); tag = SOURCE_SCALAR_TAG_BOOL; break;
                    case ORES_SCALAR_GE: value = (scalar_signed(lhs) >= scalar_signed(rhs)); tag = SOURCE_SCALAR_TAG_BOOL; break;
                    default: break;
                }
                if (!scalar_push(frame, tag, value)) goto cleanup;
                break;
            }
            case ORES_SCALAR_NOT:
                if (!scalar_pop(frame, SOURCE_SCALAR_TAG_BOOL, &rhs)
                    || !scalar_push(frame, SOURCE_SCALAR_TAG_BOOL,
                        rhs == 0)) goto cleanup;
                break;
            case ORES_SCALAR_BRANCH_FALSE:
                if (!scalar_pop(frame, SOURCE_SCALAR_TAG_BOOL, &rhs))
                    goto cleanup;
                if (!rhs) frame[4] = (uint64_t)ins.immediate;
                break;
            case ORES_SCALAR_CALL:
            case ORES_SCALAR_TAIL_CALL: {
                if (!scalar_pop(frame, SOURCE_SCALAR_TAG_I64, &rhs))
                    goto cleanup;
                const uint64_t target = (uint64_t)ins.immediate;
                if (ins.opcode == ORES_SCALAR_TAIL_CALL) {
                    scalar_clear_frame(actor, depth - 1);
                    scalar_init_frame(actor, depth - 1, target,
                        program->function_entries[target],
                        epoch, scalar_signed(rhs));
                    ++tails;
                } else {
                    if (depth == ORES_SCALAR_MAX_FRAMES) {
                        status = ORES_SCALAR_FRAME_QUOTA;
                        goto cleanup;
                    }
                    scalar_init_frame(actor, depth, target,
                        program->function_entries[target],
                        epoch, scalar_signed(rhs));
                    ++depth;
                    ++calls;
                }
                break;
            }
            case ORES_SCALAR_RETURN: {
                if (!scalar_pop(frame, SOURCE_SCALAR_TAG_I64, &rhs)
                        || frame[5] != 0) goto cleanup;
                scalar_clear_frame(actor, depth - 1);
                --depth;
                if (!depth) {
                    *result = scalar_signed(rhs);
                    status = ORES_SCALAR_OK;
                    goto cleanup;
                }
                if (!scalar_push(scalar_frame(actor, depth - 1),
                        SOURCE_SCALAR_TAG_I64, rhs)) goto cleanup;
                break;
            }
            default:
                goto cleanup;
        }
        /* Only one comparatively expensive cancellation/actor-state probe
         * per 256 dispatched instructions. Do not poll scheduler/clock or
         * acquire mutexes on the ordinary opcode fast path. */
        if ((executed & (SOURCE_SCALAR_CHECK_INTERVAL - 1u)) == 0) {
            atomic_fetch_add_explicit(&actor->frame_vm_checkpoints,
                1, memory_order_release);
#ifdef ORESLANG_SOURCE_ACTOR_TESTING
            /* The only test-only carrier pause is behind a compile flag;
             * production builds never park a carrier here. */
            if (atomic_load_explicit(&test_hold_scalar_checkpoint,
                    memory_order_acquire)) {
                atomic_store_explicit(&test_scalar_checkpoint_entered, 1,
                    memory_order_release);
                while (atomic_load_explicit(&test_hold_scalar_checkpoint,
                            memory_order_acquire)
                        && !atomic_load_explicit(&actor->stop_requested,
                            memory_order_acquire))
                    sched_yield();
            }
#endif
            if (atomic_load_explicit(&actor->stop_requested,
                    memory_order_acquire)) {
                status = ORES_SCALAR_CANCELLED;
                goto cleanup;
            }
            if (atomic_load_explicit(&actor->frame_vm_error,
                    memory_order_acquire) != ORES_SCALAR_OK)
                goto cleanup;
        }
    }
    status = ORES_SCALAR_FUEL_EXHAUSTED;
cleanup:
    while (depth) scalar_clear_frame(actor, --depth);
    /* Runtime receipts count actually dispatched scalar instructions only.
     * Quotas, invalid tags and fuel failure are actor-local terminal errors. */
    atomic_fetch_add_explicit(&actor->frame_vm_steps,
        executed, memory_order_relaxed);
    atomic_fetch_add_explicit(&actor->frame_vm_tail_calls,
        tails, memory_order_relaxed);
    atomic_fetch_add_explicit(&actor->frame_vm_non_tail_calls,
        calls, memory_order_relaxed);
    if (status == ORES_SCALAR_OK &&
            atomic_load_explicit(&actor->frame_vm_error,
                memory_order_acquire) != ORES_SCALAR_OK)
        status = ORES_SCALAR_INVALID_FRAME;
    if (status != ORES_SCALAR_OK) {
        *result = 0;
        atomic_store_explicit(&actor->frame_vm_error,
            status, memory_order_release);
    }
    atomic_store_explicit(&actor->frame_vm_active, 0,
        memory_order_release);
    return status;
}

#ifdef ORESLANG_SOURCE_ACTOR_TESTING
static atomic_int test_hold_initialization;
static atomic_int test_initialization_entered;
static atomic_int test_hold_readiness_publication;
static atomic_int test_readiness_publication_entered;

void oreslang_source_actor_test_hold_initialization(int hold) {
    if (hold) {
        atomic_store_explicit(
            &test_initialization_entered, 0, memory_order_release);
        atomic_store_explicit(
            &test_hold_initialization, 1, memory_order_release);
    } else {
        atomic_store_explicit(
            &test_hold_initialization, 0, memory_order_release);
    }
}

int oreslang_source_actor_test_initialization_entered(void) {
    return atomic_load_explicit(
        &test_initialization_entered, memory_order_acquire);
}

void oreslang_source_actor_test_hold_readiness_publication(int hold) {
    if (hold) {
        atomic_store_explicit(
            &test_readiness_publication_entered, 0, memory_order_release);
        atomic_store_explicit(
            &test_hold_readiness_publication, 1, memory_order_release);
    } else {
        atomic_store_explicit(
            &test_hold_readiness_publication, 0, memory_order_release);
    }
}

int oreslang_source_actor_test_readiness_publication_entered(void) {
    return atomic_load_explicit(
        &test_readiness_publication_entered, memory_order_acquire);
}
#endif

static void ensure(int condition) { if (!condition) abort(); }
static void group_lock(void) { ensure(pthread_mutex_lock(&group.mutex) == 0); }
static void group_unlock(void) { ensure(pthread_mutex_unlock(&group.mutex) == 0); }

static int same_allocation(
        OresNativeAllocationDomainRef a,
        OresNativeAllocationDomainRef b) {
    return a.kind == b.kind
        && a.domain == b.domain
        && a.owner == b.owner
        && a.generation == b.generation;
}

static SourceActor* lookup(size_t index) {
    ensure(index < SOURCE_GROUP_ROOT_SLOT);
    SourceActor* actor = group.actors[index];
    ensure(actor && actor->magic == SOURCE_ACTOR_MAGIC);
    return actor;
}

int oreslang_source_actor_ready_status(void* opaque) {
    SourceActor* instance = (SourceActor*)opaque;
    if (!instance || instance->magic != SOURCE_ACTOR_MAGIC) return -1;
    return atomic_load_explicit(
        &instance->startup_state, memory_order_acquire);
}

#ifdef ORESLANG_SOURCE_ACTOR_TESTING
uint64_t oreslang_source_actor_test_processed(void* opaque) {
    SourceActor* instance = (SourceActor*)opaque;
    if (!instance || instance->magic != SOURCE_ACTOR_MAGIC)
        return UINT64_MAX;
    return atomic_load_explicit(
        &instance->processed, memory_order_acquire);
}
#endif

typedef struct {
    OresNativeActorFuture* future;
    int status;
} SourceReadyFutureCommit;

static int commit_source_ready_future(void* raw) {
    SourceReadyFutureCommit* commit = (SourceReadyFutureCommit*)raw;
    return commit
        && commit->future
        && ores_native_actor_future_complete(commit->future, commit->status);
}

static void settle_ready_if_admitted_locked(ReadyAwaitFrame* frame) {
    ensure(frame && frame == group.pending_ready);
    if (!frame->suspend_recorded) return;

    const int startup_state = atomic_load_explicit(
        &frame->actor->startup_state, memory_order_acquire);
    int future_status = ORES_NATIVE_FUTURE_PENDING;
    if (startup_state == ORES_SOURCE_ACTOR_STARTUP_READY)
        future_status = ORES_NATIVE_FUTURE_READY;
    else if (startup_state
            == ORES_SOURCE_ACTOR_STARTUP_TERMINATED_BEFORE_READY)
        future_status = ORES_NATIVE_FUTURE_FAILED;
    else
        return;

    ensure(frame->ready_future != NULL);
    if (ores_native_actor_future_status(frame->ready_future)
            == ORES_NATIVE_FUTURE_PENDING) {
        /*
         * Serialize the fallible Future outcome commit and continuation READY
         * publication against cancellation under the continuation-store lock.
         * Both successful readiness and TERMINATED_BEFORE_READY must wake the
         * root CONTROL lane exactly once; only the Future outcome differs.
         */
        SourceReadyFutureCommit commit = {
            .future = frame->ready_future,
            .status = future_status
        };
        ensure(ores_native_continuation_store_commit_ready(
            group.continuations,
            frame->link.continuation,
            commit_source_ready_future,
            &commit));
    }
}

static void root_suspend_turn(void) {
    group_lock();
    ReadyAwaitFrame* frame = group.pending_ready;
    ensure(frame && !frame->done && !frame->suspend_recorded);

    OresNativeExecutionContext physical = {0};
    ensure(ores_native_vm_current_execution_context(
        group.vm, ORES_NATIVE_VM_MAIN, &physical));
    ensure(physical.allocation.kind == ORES_ALLOCATION_DOMAIN_ACTOR);
    ensure(physical.allocation.owner == SOURCE_GROUP_ROOT_SLOT);

    OresNativeExecutionHandle origin = {0, 0};
    ensure(ores_native_execution_context_store_insert(
        group.executions, &physical, &origin));
    OresNativeContinuationHandle no_parent = {0, 0};
    ensure(ores_native_await_trace_suspend(
        group.executions,
        group.continuations,
        origin,
        no_parent,
        ORES_CONTINUATION_BOUNDARY_AWAIT,
        1,
        frame->source_token,
        &frame->link));

    /*
     * The published continuation owns the durable pin. Drop this callback's
     * insertion reference before its native worker stack is torn down.
     */
    ensure(ores_native_execution_context_store_release(
        group.executions, origin));
    frame->suspend_recorded = 1;
    settle_ready_if_admitted_locked(frame);
    group_unlock();
}

static void root_resume_turn(void) {
    ReadyAwaitFrame* frame;
    OresSourceActorContinuation continuation;
    SourceActor* actor;
    OresNativeSuspensionLink link;
    int future_status;

    group_lock();
    frame = group.pending_ready;
    ensure(frame && frame->suspend_recorded && !frame->done);
    ensure(frame->ready_future != NULL);
    future_status = ores_native_actor_future_status(frame->ready_future);
    ensure(future_status == ORES_NATIVE_FUTURE_READY
        || future_status == ORES_NATIVE_FUTURE_FAILED);
    continuation = frame->continuation;
    actor = frame->actor;
    link = frame->link;
    group_unlock();

    OresNativeExecutionContext physical = {0};
    ensure(ores_native_vm_current_execution_context(
        group.vm, ORES_NATIVE_VM_MAIN, &physical));
    ensure(physical.allocation.kind == ORES_ALLOCATION_DOMAIN_ACTOR);
    ensure(physical.allocation.owner == SOURCE_GROUP_ROOT_SLOT);

    OresNativeExecutionHandle resumed = {0, 0};
    OresNativeExecutionContext logical = {0};
    ensure(ores_native_await_trace_ready_resume_begin(
        group.executions,
        group.continuations,
        physical.id,
        link,
        &resumed,
        &logical));

    /*
     * Even failed readiness resumes once on the root CONTROL lane so the
     * persisted logical suspension can be retired without leaking trace state.
     * Guest continuation code runs only for successful readiness.
     */
    ensure(same_allocation(logical.allocation, physical.allocation));
    int64_t result = 0;
    int outcome = ORES_SOURCE_ACTOR_STARTUP_TERMINATED_BEFORE_READY;
    if (future_status == ORES_NATIVE_FUTURE_READY) {
        ensure(!ores_native_store_handle_valid(current_source_execution));
        current_source_execution = resumed;
        result = continuation(actor);
        current_source_execution = (OresNativeExecutionHandle){0, 0};
        outcome = ORES_SOURCE_ACTOR_STARTUP_READY;
    }

    ensure(ores_native_await_trace_resume_finish(group.continuations, link));
    ensure(ores_native_execution_context_store_release(
        group.executions, resumed));

    group_lock();
    ensure(frame == group.pending_ready && !frame->done);
    frame->outcome = outcome;
    frame->result = result;
    frame->done = 1;
    ensure(pthread_cond_signal(&frame->completed) == 0);
    group_unlock();
}

static void root_done_suspend_turn(void) {
    group_lock();
    DoneAwaitFrame* frame = group.pending_done;
    ensure(frame && !frame->done && !frame->suspend_recorded);

    OresNativeExecutionContext physical = {0};
    ensure(ores_native_vm_current_execution_context(
        group.vm, ORES_NATIVE_VM_MAIN, &physical));
    ensure(physical.allocation.kind == ORES_ALLOCATION_DOMAIN_ACTOR);
    ensure(physical.allocation.owner == SOURCE_GROUP_ROOT_SLOT);

    OresNativeExecutionHandle origin = {0, 0};
    ensure(ores_native_execution_context_store_insert(
        group.executions, &physical, &origin));
    OresNativeContinuationHandle no_parent = {0, 0};
    ensure(ores_native_await_trace_suspend(
        group.executions,
        group.continuations,
        origin,
        no_parent,
        ORES_CONTINUATION_BOUNDARY_AWAIT,
        1,
        frame->source_token,
        &frame->link));
    ensure(ores_native_execution_context_store_release(
        group.executions, origin));

    frame->suspend_recorded = 1;
    ensure(pthread_cond_signal(&frame->completed) == 0);
    group_unlock();
}

static void root_done_resume_turn(void) {
    DoneAwaitFrame* frame;
    OresSourceActorContinuation continuation;
    SourceActor* actor;
    OresNativeSuspensionLink link;

    group_lock();
    frame = group.pending_done;
    ensure(frame && frame->suspend_recorded && !frame->done);
    ensure(frame->done_future != NULL);
    ensure(ores_native_actor_future_status(frame->done_future)
        == ORES_NATIVE_FUTURE_READY);
    continuation = frame->continuation;
    actor = frame->actor;
    link = frame->link;
    group_unlock();

    OresNativeExecutionContext physical = {0};
    ensure(ores_native_vm_current_execution_context(
        group.vm, ORES_NATIVE_VM_MAIN, &physical));
    ensure(physical.allocation.kind == ORES_ALLOCATION_DOMAIN_ACTOR);
    ensure(physical.allocation.owner == SOURCE_GROUP_ROOT_SLOT);

    OresNativeExecutionHandle resumed = {0, 0};
    OresNativeExecutionContext logical = {0};
    ensure(ores_native_await_trace_resume_begin(
        group.executions,
        group.continuations,
        physical.id,
        link,
        &resumed,
        &logical));
    ensure(same_allocation(logical.allocation, physical.allocation));
    ensure(!ores_native_store_handle_valid(current_source_execution));
    current_source_execution = resumed;
    const int64_t result = continuation(actor);
    current_source_execution = (OresNativeExecutionHandle){0, 0};

    ensure(ores_native_await_trace_resume_finish(group.continuations, link));
    ensure(ores_native_execution_context_store_release(
        group.executions, resumed));

    group_lock();
    ensure(frame == group.pending_done && !frame->done);
    frame->result = result;
    frame->done = 1;
    ensure(pthread_cond_signal(&frame->completed) == 0);
    group_unlock();
}

static void initialize(void* raw, size_t slot, uint64_t payload) {
    ensure(raw == &group);
    if (slot == SOURCE_GROUP_ROOT_SLOT) {
        if (payload == SOURCE_ROOT_CONTROL_SUSPEND) root_suspend_turn();
        else if (payload == SOURCE_ROOT_CONTROL_RESUME) root_resume_turn();
        else if (payload == SOURCE_ROOT_CONTROL_DONE_SUSPEND) root_done_suspend_turn();
        else if (payload == SOURCE_ROOT_CONTROL_DONE_RESUME) root_done_resume_turn();
        else abort();
        return;
    }

    ensure(payload == 0);
    SourceActor* instance = lookup(slot);

#ifdef ORESLANG_SOURCE_ACTOR_TESTING
    if (atomic_load_explicit(
            &test_hold_initialization, memory_order_acquire)) {
        atomic_store_explicit(
            &test_initialization_entered, 1, memory_order_release);
        while (atomic_load_explicit(
                &test_hold_initialization, memory_order_acquire))
            sched_yield();
    }
#endif

    /*
     * Match Java ActorRef.stop(): startup admission and stop admission
     * linearize under the same group mutex. The reserved initialization
     * CONTROL turn survives mailbox sealing, but stop-before-start skips guest
     * construction completely.
     */
    group_lock();
    if (atomic_load_explicit(
            &instance->stop_requested, memory_order_acquire)) {
        atomic_store_explicit(
            &instance->startup_state,
            ORES_SOURCE_ACTOR_STARTUP_TERMINATED_BEFORE_READY,
            memory_order_release);
        ReadyAwaitFrame* frame = group.pending_ready;
        if (frame && frame->actor == instance)
            settle_ready_if_admitted_locked(frame);
        group_unlock();
        return;
    }

    int expected = ORES_SOURCE_ACTOR_STARTUP_PENDING;
    ensure(atomic_compare_exchange_strong_explicit(
        &instance->startup_state,
        &expected,
        ORES_SOURCE_ACTOR_STARTUP_INITIALIZING,
        memory_order_acq_rel,
        memory_order_acquire));
    atomic_store_explicit(
        &instance->constructor_started, 1, memory_order_release);
    group_unlock();

    OresActorTurn turn =
        ores_actor_i64_heap_enter(instance->heap, instance->id, 1);
    source_frame_enter(instance, turn);
    instance->constructor(instance->heap, turn.domain, turn.epoch);
    source_frame_leave(instance, turn);
    ores_actor_i64_heap_leave(instance->heap, turn);

#ifdef ORESLANG_SOURCE_ACTOR_TESTING
    if (atomic_load_explicit(
            &test_hold_readiness_publication, memory_order_acquire)) {
        atomic_store_explicit(
            &test_readiness_publication_entered, 1, memory_order_release);
        while (atomic_load_explicit(
                &test_hold_readiness_publication, memory_order_acquire))
            sched_yield();
    }
#endif

    /*
     * Stop may race an already-running constructor. Re-enter the same group
     * mutex before publishing readiness: if stop acquired it first, readiness
     * fails; if this turn publishes READY first, a later stop cannot retroactively
     * change the already-settled readiness observer.
     */
    group_lock();
    if (atomic_load_explicit(
            &instance->stop_requested, memory_order_acquire)) {
        atomic_store_explicit(
            &instance->startup_state,
            ORES_SOURCE_ACTOR_STARTUP_TERMINATED_BEFORE_READY,
            memory_order_release);
    } else {
        atomic_store_explicit(
            &instance->initialized, 1, memory_order_release);
        atomic_store_explicit(
            &instance->startup_state,
            ORES_SOURCE_ACTOR_STARTUP_READY,
            memory_order_release);
    }

    ReadyAwaitFrame* frame = group.pending_ready;
    if (frame && frame->actor == instance)
        settle_ready_if_admitted_locked(frame);
    group_unlock();
}

static void deliver(void* raw, size_t slot, uint64_t payload) {
    ensure(raw == &group && slot != SOURCE_GROUP_ROOT_SLOT);
    SourceActor* instance = lookup(slot);
    ensure(instance->on_message != NULL);
    ensure(atomic_load_explicit(
        &instance->initialized, memory_order_acquire) == 1);
    OresActorTurn turn =
        ores_actor_i64_heap_enter(instance->heap, instance->id, 1);
    source_frame_enter(instance, turn);
    const int64_t result = instance->on_message(
        instance->heap, turn.domain, turn.epoch, (int64_t)payload);
    source_frame_leave(instance, turn);
    ores_actor_i64_heap_leave(instance->heap, turn);
    atomic_store_explicit(
        &instance->last_result, result, memory_order_relaxed);
    atomic_fetch_add_explicit(
        &instance->processed, 1, memory_order_release);
    /* A guest scalar frame quota/fuel/tag violation is isolated to the
     * offending actor. Seal its future DATA after this physically completed
     * callback; the carrier and all sibling actor lanes remain alive. */
    if (atomic_load_explicit(&instance->frame_vm_error,
            memory_order_acquire) != ORES_SCALAR_OK) {
        int expected = 0;
        if (atomic_compare_exchange_strong_explicit(
                &instance->stop_requested, &expected, 1,
                memory_order_acq_rel, memory_order_acquire)) {
            ensure(ores_native_vm_stop_sealed(
                group.vm, ORES_NATIVE_VM_MAIN, instance->ref));
        }
    }
}

static void open_group_locked(void) {
    ensure(!group.vm && !group.executions && !group.continuations);
    ensure(group.live == 0 && group.next_slot == 0 && group.peak == 0);
    ensure(group.pending_ready == NULL && group.pending_done == NULL);

    /*
     * Live trace stores are process-local runtime metadata. They hold logical
     * values/handles only and outlive individual worker stacks.
     */
    group.executions =
        ores_native_execution_context_store_open(SOURCE_TRACE_EXECUTIONS);
    ensure(group.executions != NULL);
    group.continuations = ores_native_continuation_store_open(
        SOURCE_TRACE_CONTINUATIONS, group.executions);
    ensure(group.continuations != NULL);

    OresNativeVMConfig cfg = {0};
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i) {
        cfg.domains[i].actor_slots =
            i == ORES_NATIVE_VM_MAIN ? SOURCE_GROUP_SLOTS : 1;
        cfg.domains[i].mailbox_capacity =
            i == ORES_NATIVE_VM_MAIN ? SOURCE_GROUP_MAILBOX : 1;
        cfg.domains[i].carrier_workers =
            i == ORES_NATIVE_VM_MAIN ? SOURCE_GROUP_CARRIERS : 1;
        cfg.domains[i].on_message = deliver;
        cfg.domains[i].on_control = initialize;
        cfg.domains[i].context = &group;
    }
    group.vm = ores_native_vm_open(&cfg);
    ensure(group.vm != NULL);
    /* Match the Java VM CONTROL bootstrap barrier: one native master loop is
     * running before the root actor can admit work. It only signals scheduler
     * conditions; constructor/guest execution stays on bounded carriers. */
    ensure(ores_native_vm_start_main_control_loop(group.vm));

    /*
     * The compiler-generated top-level continuation owns one reserved logical
     * actor lane. It is metadata-only until an await schedules CONTROL.
     */
    group.root_ref = ores_native_vm_spawn(
        group.vm, ORES_NATIVE_VM_MAIN, SOURCE_GROUP_ROOT_SLOT);
    ensure(group.root_ref.scheduler_domain
        && group.root_ref.actor_slot == SOURCE_GROUP_ROOT_SLOT);

#ifdef ORESLANG_SOURCE_ACTOR_TESTING
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i)
        ensure(ores_native_vm_started_carriers(
            group.vm, (enum OresNativeVMDomain)i) == 0);
    fprintf(stderr,
        "SOURCE_ACTOR_VM_ROUTING_VERIFIED domains=4 main=1 cold_carriers=0 root_slot=%u\n",
        SOURCE_GROUP_ROOT_SLOT);
#endif
}

void* oreslang_source_actor_spawn(
        OresSourceActorInit constructor,
        OresSourceActorMessage on_message,
        uint64_t fields) {
    ensure(constructor != NULL && fields >= 1 && fields <= 4096);
    group_lock();
    if (!group.vm) open_group_locked();

    ensure(group.next_slot < SOURCE_GROUP_ROOT_SLOT);
    const size_t slot = group.next_slot++;
    SourceActor* instance = (SourceActor*)calloc(1, sizeof(*instance));
    ensure(instance != NULL);
    instance->magic = SOURCE_ACTOR_MAGIC;
    instance->id = reserve_source_actor_id();
    ensure(instance->id != 0);
    instance->constructor = constructor;
    instance->on_message = on_message;

    /*
     * SourceActor is dynamically allocated. Initialize every C11 atomic
     * explicitly instead of relying on calloc byte patterns to stand in for
     * atomic object initialization.
     */
    atomic_init(&instance->initialized, 0);
    atomic_init(&instance->frame_active, 0);
    atomic_init(&instance->frame_vm_error, ORES_SCALAR_OK);
    atomic_init(&instance->frame_vm_active, 0);
    atomic_init(&instance->frame_vm_checkpoints, 0);
    atomic_init(&instance->frame_vm_steps, 0);
    atomic_init(&instance->frame_vm_tail_calls, 0);
    atomic_init(&instance->frame_vm_non_tail_calls, 0);
    atomic_init(
        &instance->startup_state, ORES_SOURCE_ACTOR_STARTUP_PENDING);
    atomic_init(&instance->constructor_started, 0);
    atomic_init(&instance->stop_requested, 0);
    atomic_init(&instance->join_started, 0);
    atomic_init(&instance->processed, 0);
    atomic_init(&instance->last_result, 0);

    instance->heap = ores_actor_i64_heap_open(instance->id, 1, fields);
    instance->frame_words = (uint64_t*)calloc(SOURCE_ACTOR_FRAME_WORDS,
        sizeof(*instance->frame_words));
    ensure(instance->frame_words != NULL);
    group.actors[slot] = instance;
    instance->ref =
        ores_native_vm_spawn(group.vm, ORES_NATIVE_VM_MAIN, slot);
    ensure(instance->ref.scheduler_domain
        && instance->ref.actor_slot == slot);
    group.live++;
    if (group.live > group.peak) group.peak = group.live;

    /* CONTROL initializes the compiler-emitted actor constructor on a worker. */
    ensure(ores_native_vm_publish_control(
        group.vm, ORES_NATIVE_VM_MAIN, instance->ref, 0));
    group_unlock();
    return instance;
}

void oreslang_source_actor_send(void* opaque, int64_t payload) {
    SourceActor* instance = (SourceActor*)opaque;
    ensure(instance && instance->magic == SOURCE_ACTOR_MAGIC
        && instance->on_message);
    ensure(ores_native_vm_try_send(
        group.vm, ORES_NATIVE_VM_MAIN, instance->ref, (uint64_t)payload)
        == ORES_NATIVE_SEND_ACCEPTED);
}

int oreslang_source_actor_await_ready_checked(
        void* opaque,
        OresSourceActorContinuation continuation,
        uint64_t source_token,
        int64_t* result_out) {
    SourceActor* instance = (SourceActor*)opaque;
    ensure(instance && instance->magic == SOURCE_ACTOR_MAGIC);
    ensure(continuation != NULL && result_out != NULL);

    /*
     * This outer wait is trusted top-level ABI glue, never guest-carrier
     * blocking. A carrier call would be a deadlock/semantic violation.
     */
    OresNativeExecutionContext carrier = {0};
    ensure(!ores_native_vm_current_execution_context(
        group.vm, ORES_NATIVE_VM_MAIN, &carrier));

    ReadyAwaitFrame frame = {
        .continuation = continuation,
        .actor = instance,
        .source_token = source_token
    };
    ensure(pthread_cond_init(&frame.completed, NULL) == 0);

    group_lock();
    ensure(group.vm && group.root_ref.scheduler_domain
        && group.pending_ready == NULL && group.pending_done == NULL);
    frame.ready_future = ores_native_vm_actor_future_open(
        group.vm,
        ORES_NATIVE_VM_MAIN,
        group.root_ref,
        SOURCE_ROOT_CONTROL_RESUME);
    ensure(frame.ready_future != NULL);
    group.pending_ready = &frame;

    /*
     * First root turn captures/suspends its scheduler-minted ExecutionId.
     * Future settlement cannot resume until this CONTROL turn has published
     * the OresNativeSuspensionLink.
     */
    ensure(ores_native_vm_publish_control(
        group.vm,
        ORES_NATIVE_VM_MAIN,
        group.root_ref,
        SOURCE_ROOT_CONTROL_SUSPEND));

    while (!frame.done)
        ensure(pthread_cond_wait(&frame.completed, &group.mutex) == 0);

    ensure(group.pending_ready == &frame);
    group.pending_ready = NULL;
    const int outcome = frame.outcome;
    if (outcome == ORES_SOURCE_ACTOR_STARTUP_READY)
        *result_out = frame.result;

    ores_native_actor_future_destroy(frame.ready_future);
    ensure(ores_native_continuation_store_release(
        group.continuations, frame.link.continuation));
    ensure(ores_native_execution_context_store_live(group.executions) == 0);
    ensure(ores_native_continuation_store_live(group.continuations) == 0);
    group_unlock();

    ensure(pthread_cond_destroy(&frame.completed) == 0);
#ifdef ORESLANG_SOURCE_ACTOR_TESTING
    fprintf(stderr,
        "SOURCE_AWAIT_READY_TRACE_PASS outcome=%d execution_id_persisted=1 fresh_resume=1 root_slot=%u source_token=%llu\n",
        outcome,
        SOURCE_GROUP_ROOT_SLOT,
        (unsigned long long)source_token);
#endif
    return outcome;
}

int64_t oreslang_source_actor_await_ready(
        void* opaque,
        OresSourceActorContinuation continuation,
        uint64_t source_token) {
    int64_t result = 0;
    const int outcome = oreslang_source_actor_await_ready_checked(
        opaque, continuation, source_token, &result);
    /*
     * Existing source lowering has no typed ActorTerminated projection yet.
     * Fail closed instead of hanging or running the continuation after failed
     * readiness; checked host/runtime callers can observe the failure exactly.
     */
    ensure(outcome == ORES_SOURCE_ACTOR_STARTUP_READY);
    return result;
}

int64_t oreslang_source_actor_await_done(
        void* opaque,
        OresSourceActorContinuation continuation,
        uint64_t source_token) {
    SourceActor* instance = (SourceActor*)opaque;
    ensure(instance && instance->magic == SOURCE_ACTOR_MAGIC);
    ensure(continuation != NULL);
    ensure(atomic_load_explicit(
        &instance->stop_requested, memory_order_acquire) == 1);
    ensure(atomic_load_explicit(
        &instance->join_started, memory_order_acquire) == 0);

    /*
     * The physical wait is trusted top-level glue. It must never park an
     * actor carrier: the target actor may need that carrier to drain accepted
     * work and publish its terminal acknowledgement.
     */
    OresNativeExecutionContext carrier = {0};
    ensure(!ores_native_vm_current_execution_context(
        group.vm, ORES_NATIVE_VM_MAIN, &carrier));

    DoneAwaitFrame frame = {
        .continuation = continuation,
        .actor = instance,
        .source_token = source_token
    };
    ensure(pthread_cond_init(&frame.completed, NULL) == 0);

    group_lock();
    ensure(group.vm && group.root_ref.scheduler_domain
        && group.pending_ready == NULL && group.pending_done == NULL);
    OresNativeVM* vm = group.vm;
    const OresNativeActorRef target = instance->ref;
    frame.done_future = ores_native_vm_actor_future_open(
        vm,
        ORES_NATIVE_VM_MAIN,
        group.root_ref,
        SOURCE_ROOT_CONTROL_DONE_RESUME);
    ensure(frame.done_future != NULL);
    group.pending_done = &frame;

    /*
     * Persist logical ancestry on a root CONTROL turn before waiting on the
     * target. The host waits for this acknowledgement so no continuation is
     * completed before its durable suspension link exists.
     */
    ensure(ores_native_vm_publish_control(
        vm,
        ORES_NATIVE_VM_MAIN,
        group.root_ref,
        SOURCE_ROOT_CONTROL_DONE_SUSPEND));
    while (!frame.suspend_recorded)
        ensure(pthread_cond_wait(&frame.completed, &group.mutex) == 0);
    group_unlock();

    ensure(ores_native_vm_wait_done(
        vm, ORES_NATIVE_VM_MAIN, target));

    group_lock();
    ensure(group.vm == vm && group.pending_done == &frame);
    ensure(ores_native_vm_poll_done(
        vm, ORES_NATIVE_VM_MAIN, target));
    ensure(ores_native_actor_future_status(frame.done_future)
        == ORES_NATIVE_FUTURE_PENDING);
    ensure(ores_native_actor_future_complete(
        frame.done_future, ORES_NATIVE_FUTURE_READY));

    while (!frame.done)
        ensure(pthread_cond_wait(&frame.completed, &group.mutex) == 0);

    ensure(group.pending_done == &frame);
    group.pending_done = NULL;
    const int64_t result = frame.result;

    ores_native_actor_future_destroy(frame.done_future);
    ensure(ores_native_continuation_store_release(
        group.continuations, frame.link.continuation));
    ensure(ores_native_execution_context_store_live(group.executions) == 0);
    ensure(ores_native_continuation_store_live(group.continuations) == 0);
    group_unlock();

    ensure(pthread_cond_destroy(&frame.completed) == 0);
#ifdef ORESLANG_SOURCE_ACTOR_TESTING
    fprintf(stderr,
        "SOURCE_AWAIT_DONE_TRACE_PASS terminal_ack=1 execution_id_persisted=1 fresh_resume=1 root_slot=%u source_token=%llu\n",
        SOURCE_GROUP_ROOT_SLOT,
        (unsigned long long)source_token);
#endif

    /*
     * Reclamation is deliberately after the resumed continuation. This keeps
     * the opaque ActorRef allocation valid while the continuation is active,
     * but no guest message/stop capability is admitted after get_done.
     */
    oreslang_source_actor_join_done(instance);
    return result;
}

int oreslang_source_actor_current_execution_handle(
        OresNativeExecutionHandle* out) {
    if (!out || !ores_native_store_handle_valid(current_source_execution))
        return 0;
    *out = current_source_execution;
    return 1;
}

int oreslang_source_actor_snapshot_current_trace(
        OresNativeExecutionTraceFrame* execution_frames,
        size_t execution_capacity,
        OresNativeContinuationTraceFrame* continuation_frames,
        size_t continuation_capacity,
        OresNativeLogicalTraceResult* out) {
    if (!out || !ores_native_store_handle_valid(current_source_execution)
            || !group.executions || !group.continuations)
        return 0;
    return ores_native_logical_trace_snapshot(
        group.executions,
        group.continuations,
        current_source_execution,
        execution_frames,
        execution_capacity,
        continuation_frames,
        continuation_capacity,
        out);
}

void oreslang_source_actor_request_stop(void* opaque) {
    SourceActor* instance = (SourceActor*)opaque;
    ensure(instance && instance->magic == SOURCE_ACTOR_MAGIC);

    /*
     * Stop admission shares the group mutex with startup/readiness publication
     * so exactly one side wins the stop-vs-ready linearization point.
     * This phase still never waits for physical completion.
     */
    group_lock();
    int expected = 0;
    const int won_request = atomic_compare_exchange_strong_explicit(
        &instance->stop_requested,
        &expected,
        1,
        memory_order_acq_rel,
        memory_order_acquire);
    /* Automatic fail-stop is the one authorized source of a previously
     * requested stop. Manual double-stop remains a hard protocol failure. */
    ensure(won_request || (expected == 1 &&
        atomic_load_explicit(&instance->frame_vm_error,
            memory_order_acquire) != ORES_SCALAR_OK));

    const size_t slot = (size_t)instance->ref.actor_slot;
    ensure(group.vm && slot < group.next_slot
        && group.actors[slot] == instance);
    /*
     * Source stop matches Java ActorRef.stop(), not gracefulStop(): reject
     * future DATA and abandon queued DATA, while an already-active turn is
     * allowed to cross its physical execution boundary.
     */
    if (won_request)
        ensure(ores_native_vm_stop_sealed(
            group.vm, ORES_NATIVE_VM_MAIN, instance->ref));
    group_unlock();
}

void oreslang_source_actor_join_done(void* opaque) {
    SourceActor* instance = (SourceActor*)opaque;
    ensure(instance && instance->magic == SOURCE_ACTOR_MAGIC);
    ensure(atomic_load_explicit(
        &instance->stop_requested, memory_order_acquire) == 1);

    int expected = 0;
    ensure(atomic_compare_exchange_strong_explicit(
        &instance->join_started,
        &expected,
        1,
        memory_order_acq_rel,
        memory_order_acquire));

    /*
     * This is trusted outer-host glue, never a carrier turn. Do not block a
     * source actor carrier while waiting for the actor's physical terminal ACK.
     */
    OresNativeExecutionContext carrier = {0};
    ensure(!ores_native_vm_current_execution_context(
        group.vm, ORES_NATIVE_VM_MAIN, &carrier));

    const size_t slot = (size_t)instance->ref.actor_slot;
    ensure(ores_native_vm_wait_done(
        group.vm, ORES_NATIVE_VM_MAIN, instance->ref));

    group_lock();
    ensure(group.vm && slot < group.next_slot
        && group.actors[slot] == instance);
    const int startup_state = atomic_load_explicit(
        &instance->startup_state, memory_order_acquire);
    ensure(startup_state == ORES_SOURCE_ACTOR_STARTUP_READY
        || startup_state
            == ORES_SOURCE_ACTOR_STARTUP_TERMINATED_BEFORE_READY);
    ensure(ores_native_vm_poll_done(
        group.vm, ORES_NATIVE_VM_MAIN, instance->ref));

    OresNativeActorTerminalState terminal = {0};
    ensure(ores_native_vm_actor_terminal_state(
        group.vm, ORES_NATIVE_VM_MAIN, instance->ref, &terminal));
    ensure(terminal.kind == ORES_NATIVE_TERMINATION_STOPPED);
    ensure(terminal.processed_user_messages
        + terminal.abandoned_user_messages
        == terminal.accepted_user_messages);
    ensure(terminal.processed_user_messages
        == atomic_load_explicit(&instance->processed, memory_order_acquire));
    ensure(terminal.abandoned_control_messages == 0);

    /*
     * get_done parity requires physical quiescence, not a logical stop bit.
     * Reclaim the actor-owned heap only after the VM terminal acknowledgement.
     */
    ensure(atomic_load_explicit(&instance->frame_active,
        memory_order_acquire) == 0);
    ores_actor_i64_heap_retire(instance->heap);
    ensure(ores_actor_i64_heap_try_destroy(instance->heap));
    /* Physical scheduler completion forbids any further frame access. */
    volatile uint64_t* frame = (volatile uint64_t*)instance->frame_words;
    for (size_t i = 0; i < SOURCE_ACTOR_FRAME_WORDS; ++i) frame[i] = 0;
    free(instance->frame_words);
    instance->frame_words = NULL;
#ifdef ORESLANG_SOURCE_ACTOR_TESTING
    fprintf(stderr,
        "SOURCE_ACTOR_FRAME_RETIRE_VERIFIED actor=%llu bytes=65536 high_water=%llu slot_calls=%llu released=1\n",
        (unsigned long long)instance->id,
        (unsigned long long)instance->frame_high_water,
        (unsigned long long)instance->frame_slot_calls);
    fprintf(stderr,
        "SOURCE_ACTOR_SCALAR_VM_VERIFIED actor=%llu steps=%llu tail_calls=%llu non_tail_calls=%llu error=%d\n",
        (unsigned long long)instance->id,
        (unsigned long long)atomic_load_explicit(
            &instance->frame_vm_steps, memory_order_acquire),
        (unsigned long long)atomic_load_explicit(
            &instance->frame_vm_tail_calls, memory_order_acquire),
        (unsigned long long)atomic_load_explicit(
            &instance->frame_vm_non_tail_calls, memory_order_acquire),
        atomic_load_explicit(&instance->frame_vm_error,
            memory_order_acquire));
    fprintf(stderr,
        "SOURCE_ACTOR_SPAWN_STOP_VERIFIED constructor_on_worker=%d released=1 ready=%d accepted=%llu processed=%llu abandoned=%llu last=%lld\n",
        atomic_load_explicit(
            &instance->constructor_started, memory_order_acquire),
        startup_state == ORES_SOURCE_ACTOR_STARTUP_READY,
        (unsigned long long)terminal.accepted_user_messages,
        (unsigned long long)terminal.processed_user_messages,
        (unsigned long long)terminal.abandoned_user_messages,
        (long long)atomic_load_explicit(
            &instance->last_result, memory_order_relaxed));
#endif
    group.actors[slot] = NULL;
    instance->magic = 0;
    free(instance);
    ensure(group.live > 0);
    group.live--;

    if (group.live == 0) {
        ensure(group.pending_ready == NULL && group.pending_done == NULL);
        ensure(ores_native_execution_context_store_live(group.executions) == 0);
        ensure(ores_native_continuation_store_live(group.continuations) == 0);

        ensure(ores_native_vm_close(group.vm));
        group.vm = NULL;
        group.root_ref = (OresNativeActorRef){0, 0, 0};

        ensure(ores_native_continuation_store_close(group.continuations));
        group.continuations = NULL;
        ensure(ores_native_execution_context_store_close(group.executions));
        group.executions = NULL;
#ifdef ORESLANG_SOURCE_ACTOR_TESTING
        fprintf(stderr,
            "SOURCE_ACTOR_GROUP_VERIFIED active=0 slots=%zu peak=%zu carriers=%u\n",
            group.next_slot, group.peak, SOURCE_GROUP_CARRIERS);
#endif
        group.next_slot = 0;
        group.peak = 0;
    }
    group_unlock();
}

void oreslang_source_actor_stop(void* opaque) {
    /*
     * Preserve synchronous stop for source paths that do not await get_done.
     * get_done lowering instead emits request_stop followed by await_done.
     */
    oreslang_source_actor_request_stop(opaque);
    oreslang_source_actor_join_done(opaque);
}
