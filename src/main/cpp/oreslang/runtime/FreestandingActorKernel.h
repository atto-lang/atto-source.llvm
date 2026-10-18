#ifndef ORESLANG_FREESTANDING_ACTOR_KERNEL_H
#define ORESLANG_FREESTANDING_ACTOR_KERNEL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Prototype freestanding CONTROL scheduler, not the hosted NativeOresVM.
 * OresKernel is caller-owned static storage; no OS/libc/heap/atomics/threads.
 * Deliberately single-core, cooperative and non-reentrant. It does not yet
 * adapt LLVM's SourceActorBootstrap ABI, permit async/await, or enforce MPU.
 */
#define ORES_KERNEL_ACTORS 8u
#define ORES_KERNEL_MAILBOX 16u
#define ORES_KERNEL_FIELDS 4u
#define ORES_KERNEL_MAGIC UINT32_C(0x4f52414b)

typedef struct OresKernel OresKernel;
typedef struct { uint32_t slot; uint32_t generation; } OresKernelRef;
/* Invoked only by the trusted executor. The field pointer belongs to the
 * current actor and must not be retained/shared outside the callback. */
typedef int (*OresKernelHandler)(uint64_t fields[ORES_KERNEL_FIELDS],
                                  int64_t message, int64_t *result);
typedef enum {
    ORES_KERNEL_OK = 0,
    ORES_KERNEL_INVALID = 1,
    ORES_KERNEL_FULL = 2,
    ORES_KERNEL_CLOSED = 3,
    ORES_KERNEL_BUSY = 4,
    ORES_KERNEL_FAULT = 5
} OresKernelStatus;
typedef enum {
    ORES_KERNEL_IDLE = 0,
    ORES_KERNEL_DELIVERED = 1,
    ORES_KERNEL_RETIRED = 2,
    ORES_KERNEL_HANDLER_FAILED = 3,
    ORES_KERNEL_REENTRANT = 4
} OresKernelTick;

typedef struct {
    uint32_t generation;
    uint32_t active;
    uint32_t closing;
    uint32_t head;
    uint32_t length;
    uint64_t fields[ORES_KERNEL_FIELDS];
    int64_t messages[ORES_KERNEL_MAILBOX];
    OresKernelHandler handler;
    uint64_t processed;
    int64_t last_result;
} OresKernelActor;
struct OresKernel {
    uint32_t magic;
    uint32_t next_slot;
    uint32_t executing;
    uint32_t fault_count;
    OresKernelActor actors[ORES_KERNEL_ACTORS];
};

/* The caller must provide zero-initialized storage before first init.
 * Subsequent init invalidates prior generations; no reset during a turn. */
OresKernelStatus ores_kernel_init(OresKernel *kernel);
OresKernelStatus ores_kernel_spawn(OresKernel *kernel,
                                   OresKernelHandler handler,
                                   OresKernelRef *out);
OresKernelStatus ores_kernel_send(OresKernel *kernel, OresKernelRef ref,
                                  int64_t payload);
OresKernelStatus ores_kernel_stop(OresKernel *kernel, OresKernelRef ref);
OresKernelTick ores_kernel_tick(OresKernel *kernel);
/* Read-only CONTROL snapshot: no pointers to private actor state escape. */
OresKernelStatus ores_kernel_snapshot(const OresKernel *kernel, OresKernelRef ref,
                                      uint64_t *processed, int64_t *last_result);
#ifdef __cplusplus
}
#endif
#endif
