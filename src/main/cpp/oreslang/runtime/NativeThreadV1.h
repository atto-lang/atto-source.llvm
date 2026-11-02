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
    ORES_THREAD_SLOT_LIMIT_V1 = 7
} ores_thread_status_v1;

/* On successful spawn, owned_arg moves to the thread and drop_arg runs ONCE
 * after entry returns. On failure the caller still owns owned_arg. */
ores_thread_status_v1 ores_native_thread_spawn_v1(
    ores_native_thread_entry_v1 entry, void *owned_arg,
    ores_native_thread_drop_v1 drop_arg, ores_native_thread_v1 **out);

/* Join exactly once; a cancelled async waiter must not invoke this itself.
 * This blocking HOST primitive MUST NOT be called on an AttoLang carrier. */
ores_thread_status_v1 ores_native_thread_join_blocking_v1(
    ores_native_thread_v1 *handle, int *out_result);
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

#ifdef __cplusplus
}
#endif
#endif
