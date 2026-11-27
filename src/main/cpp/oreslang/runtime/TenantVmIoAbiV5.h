#pragma once
/*
 * CANDIDATE guest ABI v5 for asynchronous host-mediated filesystem calls.
 * ABI v4 (TenantVmGuestAbi.h) remains unchanged and is the only version
 * accepted by the current TenantVM loader. Merely including this header
 * DOES NOT enable v5 guest modules or authorize any operation.
 *
 * The guest supplies only checked Offset32 values and ticket numbers.
 * The host derives tenant, actor generation, memory region, and capabilities
 * from the CURRENT SCHEDULER TURN; none is provided by the guest.
 */
#include "oreslang/runtime/TenantVmGuestAbi.h"
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define ORES_VM_GUEST_IO_ABI_VERSION 5u
#define ORES_VM_IO_MAX_BYTES 1048576u

/* Candidate canonical guest-visible statuses. Nonnegative poll values are
 * byte lengths; PENDING is not a completed operation or successful result. */
#define ORES_VM_IO_PENDING (-1LL)
#define ORES_VM_IO_PERMISSION_DENIED (-2LL)
#define ORES_VM_IO_CANCELLED (-3LL)
#define ORES_VM_IO_STALE_TICKET (-4LL)
#define ORES_VM_IO_TOO_LARGE (-5LL)
#define ORES_VM_IO_HOST_ERROR (-6LL)
#define ORES_VM_IO_UNSUPPORTED (-7LL)
/* A cancelled write was already executing: it MAY have committed. This is
 * distinct from a safely cancelled queued request, never auto-retry it. */
#define ORES_VM_IO_WRITE_INDETERMINATE (-8LL)
/* Transient queue/service contention on SUBMIT or CANCEL: retry later.
 * This is distinct from a poll's PENDING (an accepted ticket in flight). */
#define ORES_VM_IO_WOULD_BLOCK (-9LL)
/* Cancelling a job that is already done: poll the original ticket. */
#define ORES_VM_IO_ALREADY_DONE (-10LL)

typedef struct OresVmHostApiV5 {
    /* Exact ABI-v4 prefix, never a reinterpretation of a v4-sized table. */
    OresVmHostApi v4;
    /*
     * Asynchronous submit calls return a positive opaque ticket or a negative
     * failure. No pathname or payload pointers leave the guest, only Offset32
     * byte ranges copied into host-owned bounded request storage.
     * Submit must NEVER park a scheduler carrier on filesystem I/O.
     */
    int64_t (*fs_submit_read)(uint32_t path_offset, uint32_t path_length,
                              uint32_t maximum_bytes);
    int64_t (*fs_submit_write)(uint32_t path_offset, uint32_t path_length,
                               uint32_t payload_offset, uint32_t payload_bytes);
    /*
     * Poll may copy completed read bytes to an independently verified
     * current-actor output span. It MUST recheck revocation, origin actor,
     * incarnation, and offset bounds before ANY copy, atomically with
     * permission revocation. Returns PENDING until completion, then
     * nonnegative bytes or a negative typed status. Tickets are one-shot.
     */
    int64_t (*fs_poll)(uint64_t ticket, uint32_t output_offset,
                       uint32_t output_capacity);
    /*
     * Request cancellation, never claim that an already running write did
     * not commit. Cancelled requests must finish exactly once; no stale
     * ticket can resume a different actor incarnation.
     */
    int32_t (*fs_cancel)(uint64_t ticket);
} OresVmHostApiV5;

typedef int64_t (*OresVmGuestTurnV5)(const OresVmHostApiV5 *host,
                                     void *instance, uint8_t *cage, uint64_t self,
                                     uint32_t state, uint32_t input,
                                     uint32_t input_length, uint32_t output,
                                     uint32_t output_capacity);

typedef struct OresVmGuestKindV5 {
    OresVmGuestTurnV5 turn;
    uint32_t state_bytes;
    uint32_t heap_bytes;
    uint32_t scratch_bytes;
    uint32_t instance_bytes;
    uint32_t stored_bytes;
    uint32_t flags;
} OresVmGuestKindV5;

typedef struct OresVmGuestModuleV5 {
    uint32_t abi_version; /* must equal ORES_VM_GUEST_IO_ABI_VERSION */
    uint32_t kind_count;
    const OresVmGuestKindV5 *kinds;
} OresVmGuestModuleV5;

/* Existing module loader rejects version 5 until an authenticated, reviewed
 * v5 host dispatch is installed. No implicit v4 fallthrough is permitted. */
#if defined(__cplusplus)
static_assert(offsetof(OresVmHostApiV5, v4) == 0, "ABI v4 prefix");
static_assert(sizeof(OresVmGuestKindV5) == sizeof(OresVmGuestKind), "kind ABI layout");
static_assert(sizeof(OresVmGuestModuleV5) == sizeof(OresVmGuestModule), "module ABI layout");
#else
_Static_assert(offsetof(OresVmHostApiV5, v4) == 0, "ABI v4 prefix");
_Static_assert(sizeof(OresVmGuestKindV5) == sizeof(OresVmGuestKind), "kind ABI layout");
_Static_assert(sizeof(OresVmGuestModuleV5) == sizeof(OresVmGuestModule), "module ABI layout");
#endif
#ifdef __cplusplus
}
#endif
