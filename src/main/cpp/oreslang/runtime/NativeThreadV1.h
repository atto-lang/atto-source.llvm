#ifndef ORESLANG_NATIVE_THREAD_V1_H
#define ORESLANG_NATIVE_THREAD_V1_H

#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Host-only POSIX substrate. The AttoLang compiler must enforce Send/Sync,
 * owned captures, thread capability and nonblocking join_async separately.
 * No guest frontend or code generation is provided by this header. */
typedef struct ores_native_thread_v1 ores_native_thread_v1;
typedef struct ores_native_thread_scope_v1 ores_native_thread_scope_v1;
typedef void (*ores_native_thread_drop_v1)(void *);
typedef int (*ores_native_thread_entry_v1)(void *owned_arg);

typedef enum ores_thread_status_v1 {
    ORES_THREAD_OK_V1 = 0,
    ORES_THREAD_BAD_ARGUMENT_V1 = 1,
    ORES_THREAD_NO_CONTEXT_V1 = 2,
    ORES_THREAD_OUT_OF_MEMORY_V1 = 3,
    ORES_THREAD_OS_ERROR_V1 = 4,
    ORES_THREAD_ALREADY_JOINED_V1 = 5,
    ORES_THREAD_SELF_JOIN_V1 = 6,
    ORES_THREAD_SLOT_LIMIT_V1 = 7,
    ORES_THREAD_UNEXPECTED_EXIT_V1 = 8,
    ORES_THREAD_SCOPE_FULL_V1 = 9,
    ORES_THREAD_SCOPE_CLOSED_V1 = 10,
    ORES_THREAD_SCOPE_BUSY_V1 = 11,
    ORES_THREAD_SCOPE_IDENTITY_MISMATCH_V1 = 12
} ores_thread_status_v1;

/* On successful spawn, owned_arg moves to the thread and drop_arg runs ONCE
 * after entry returns. On failure the caller still owns owned_arg. */
ores_thread_status_v1 ores_native_thread_spawn_v1(
    ores_native_thread_entry_v1 entry, void *owned_arg,
    ores_native_thread_drop_v1 drop_arg, ores_native_thread_v1 **out);

/* Host-only per-isolate native-thread capability. 1..1024 outstanding
 * handles, not just executing workers: even a finished pthread still
 * consumes capacity until physical join + release. Separate scopes cannot
 * borrow capacity from each other. The compiler must never export the
 * raw unscoped spawn to untrusted guest code.
 *
 * Every operation requires a live scope and external isolation ownership.
 * close is nonblocking: it irrevocably stops admission, returning BUSY
 * until all joined handles have been released. destroy succeeds only after
 * close + physical quiescence, and the owner must first fence all concurrent
 * callers. Failed spawn does not consume owned_arg. */
ores_thread_status_v1 ores_native_thread_scope_open_v1(
    unsigned capacity, ores_native_thread_scope_v1 **out);
ores_thread_status_v1 ores_native_thread_scope_spawn_v1(
    ores_native_thread_scope_v1 *scope,
    ores_native_thread_entry_v1 entry, void *owned_arg,
    ores_native_thread_drop_v1 drop_arg, ores_native_thread_v1 **out);
ores_thread_status_v1 ores_native_thread_scope_close_v1(
    ores_native_thread_scope_v1 *scope);
ores_thread_status_v1 ores_native_thread_scope_outstanding_v1(
    ores_native_thread_scope_v1 *scope, unsigned *out);
ores_thread_status_v1 ores_native_thread_scope_destroy_v1(
    ores_native_thread_scope_v1 *scope);

/* Host-minted isolate routing identity (not a bearer secret).
 * The native scheduler/IsolateVM must retain the opaque scope and supply
 * an authenticated expected identity from its own execution context.
 * Guest-controlled values must NEVER be accepted as this expected key.
 * Forbid reusing a scope after a generation change; never confuse tenant
 * equality with authority to access native thread resources.
 *
 * The older unkeyed v1 functions remain host ABI compatibility primitives,
 * NOT compiler/guest entry points. Scoped compiler lowering must use the
 * checked API below and preserve external caller-fencing on destruction. */
typedef struct ores_native_thread_isolate_key_v1 {
    uint64_t tenant_id;
    uint64_t isolate_id;
    uint64_t generation;
} ores_native_thread_isolate_key_v1;

ores_thread_status_v1 ores_native_thread_scope_open_isolate_v1(
    ores_native_thread_isolate_key_v1 key, unsigned capacity,
    ores_native_thread_scope_v1 **out);
ores_thread_status_v1 ores_native_thread_scope_spawn_isolate_v1(
    ores_native_thread_scope_v1 *scope,
    ores_native_thread_isolate_key_v1 expected,
    ores_native_thread_entry_v1 entry, void *owned_arg,
    ores_native_thread_drop_v1 drop_arg, ores_native_thread_v1 **out);
ores_thread_status_v1 ores_native_thread_scope_close_isolate_v1(
    ores_native_thread_scope_v1 *scope,
    ores_native_thread_isolate_key_v1 expected);
ores_thread_status_v1 ores_native_thread_scope_destroy_isolate_v1(
    ores_native_thread_scope_v1 *scope,
    ores_native_thread_isolate_key_v1 expected);

/* Join exactly once. Unexpected pthread_exit/cancel is reported AFTER a
 * successful physical pthread_join, so the caller must still release the
 * consumed handle. A cancelled async waiter must not invoke this itself.
 * This blocking HOST primitive MUST NOT be called on an AttoLang carrier. */
ores_thread_status_v1 ores_native_thread_join_blocking_v1(
    ores_native_thread_v1 *handle, int *out_result);
/* Advisory guest-body/TLS completion bit for scheduler notifications. A true
 * value does NOT mean pthread_join has reaped the native thread; a separate
 * host reaper must perform join. Polling is not an efficient wake mechanism. */
ores_thread_status_v1 ores_native_thread_poll_completed_v1(
    const ores_native_thread_v1 *handle, int *out_completed);

ores_thread_status_v1 ores_native_thread_release_v1(
    ores_native_thread_v1 *joined_handle);

/* Physical-thread storage. Key 0 is invalid; 64 slots per admitted thread.
 * get yields a non-owning pointer only until the next set/take/exit.
 * set transfers ownership only on success. take returns owned pointer + drop.
 * Neither API has any task- or actor-local fallback. */
ores_thread_status_v1 ores_native_thread_local_set_v1(
    uint64_t key, void *owned_value, ores_native_thread_drop_v1 drop);
ores_thread_status_v1 ores_native_thread_local_get_v1(
    uint64_t key, void **borrowed_value);
ores_thread_status_v1 ores_native_thread_local_take_v1(
    uint64_t key, void **owned_value, ores_native_thread_drop_v1 *drop);
ores_thread_status_v1 ores_native_thread_id_v1(uint64_t *out_id);

/* Compile-time-only boundary for isolated thread-ID exhaustion regression. */
#ifdef ORES_NATIVE_THREAD_TEST_ID_EXHAUSTION
void ores_native_thread_test_set_next_id_v1(uint64_t id);
#endif

#ifdef __cplusplus
}
#endif
#endif
