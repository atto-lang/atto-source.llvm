#pragma once
#include "oreslang/runtime/NativeActorScheduler.h"
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Host-side OresVM CONTROL-plane manager; zero GC and no guest callbacks.
 * Master owns ONE eagerly started pthread. Each attached actor pool owns one
 * independent, lazily started scheduler pthread plus 0..5 worker carriers.
 * The master queues only coalesced wake signals, not message/actor ownership.
 */
typedef struct OresNativeVMMasterScheduler OresNativeVMMasterScheduler;
typedef enum {
    ORES_VM_POOL_SHARED = 0,
    ORES_VM_POOL_PRIVATE = 1,
    ORES_VM_POOL_UNTRUSTED = 2
} OresNativeVMPoolKind;

/* Starts exactly one master scheduler CONTROL thread; returns NULL on failure.
 * All memory and OS threads are released explicitly by retire+try_destroy.
 */
OresNativeVMMasterScheduler* ores_native_vm_master_open(void);
/* Attach/detach are serialized against wake delivery. Attach requires a
 * distinct native actor pool, with externally owned lifetime. A pool MUST be
 * detached before it is destroyed: detach waits for in-flight notification.
 */
int ores_native_vm_master_attach(
    OresNativeVMMasterScheduler*, OresNativeVMPoolKind,
    OresNativeActorScheduler*);
int ores_native_vm_master_detach(
    OresNativeVMMasterScheduler*, OresNativeVMPoolKind,
    OresNativeActorScheduler*);
/* Queue a coalesced wake and return without running guest code. Returns zero
 * on retirement, invalid kind or absent pool. A wake is NOT a message receipt.
 */
int ores_native_vm_master_notify(
    OresNativeVMMasterScheduler*, OresNativeVMPoolKind);
/* Diagnostics: acknowledged wake batches, not guest messages processed. */
uint64_t ores_native_vm_master_delivered_wakes(
    OresNativeVMMasterScheduler*);
size_t ores_native_vm_master_started_control_threads(
    OresNativeVMMasterScheduler*);
/* External callers must fence producers and detach pools before destruction.
 * try_destroy returns 0 if not retired or if pools remain attached.
 */
void ores_native_vm_master_retire(OresNativeVMMasterScheduler*);
int ores_native_vm_master_try_destroy(OresNativeVMMasterScheduler*);

#ifdef __cplusplus
}
#endif
