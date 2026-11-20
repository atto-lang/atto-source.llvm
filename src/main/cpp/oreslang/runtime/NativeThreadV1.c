#include "NativeThreadV1.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>

#define ORES_THREAD_MAX_SLOTS_V1 64u

typedef struct ores_slot_v1 {
    uint64_t key;
    void *value;
    ores_native_thread_drop_v1 drop;
    struct ores_slot_v1 *next;
} ores_slot_v1;

typedef struct ores_context_v1 {
    uint64_t id;
    unsigned slots_count;
    ores_slot_v1 *slots;
} ores_context_v1;

struct ores_native_thread_scope_v1 {
    pthread_mutex_t lock;
    ores_native_thread_isolate_key_v1 isolate_key; /* immutable after publish */
    unsigned capacity;
    unsigned outstanding; /* joined + released, not merely completed */
    int closing;
};

struct ores_native_thread_v1 {
    ores_native_thread_scope_v1 *scope; /* scope owns admission until release */
    pthread_t native;
    atomic_int join_state; /* 0 joinable, 1 joining, 2 joined */
    atomic_int body_completed; /* release after all TLS destructors */
    atomic_int abnormal_exit; /* no returned entry result (pthread_exit/cancel) */
    int result;
};

typedef struct ores_start_v1 {
    ores_native_thread_entry_v1 entry;
    void *arg;
    ores_native_thread_drop_v1 drop;
    ores_context_v1 *context;
    ores_native_thread_v1 *handle;
} ores_start_v1;

static pthread_key_t ores_tls_key_v1;
static pthread_once_t ores_tls_once_v1 = PTHREAD_ONCE_INIT;
static atomic_int ores_tls_result_v1 = 0;
static atomic_uint_fast64_t ores_id_counter_v1 = 1;

/* Logical native thread incarnations cannot be reused. fetch_add would
 * wrap at UINT64_MAX and eventually reissue older identities; reserve the
 * exhausted sentinel with CAS and fail closed instead. */
static uint64_t ores_allocate_thread_id_v1(void) {
    uint_fast64_t current = atomic_load_explicit(
        &ores_id_counter_v1, memory_order_relaxed);
    while (current != 0 && current < UINT64_MAX) {
        if (atomic_compare_exchange_weak_explicit(
                &ores_id_counter_v1, &current, current + 1,
                memory_order_relaxed, memory_order_relaxed))
            return (uint64_t)current;
    }
    return 0;
}

#ifdef ORES_NATIVE_THREAD_TEST_ID_EXHAUSTION
/* Test build only: forbidden in production guest/runtime objects. */
void ores_native_thread_test_set_next_id_v1(uint64_t id) {
    atomic_store_explicit(&ores_id_counter_v1, id, memory_order_relaxed);
}
#endif

static void ores_tls_init_v1(void) {
    if (pthread_key_create(&ores_tls_key_v1, NULL) != 0) {
        atomic_store(&ores_tls_result_v1, 1);
    }
}

static ores_context_v1 *ores_context_current_v1(void) {
    if (pthread_once(&ores_tls_once_v1, ores_tls_init_v1) != 0 ||
        atomic_load(&ores_tls_result_v1) != 0) {
        return NULL;
    }
    return (ores_context_v1 *)pthread_getspecific(ores_tls_key_v1);
}

static void ores_context_destroy_v1(ores_context_v1 *context) {
    ores_slot_v1 *slot = context->slots;
    while (slot) {
        ores_slot_v1 *next = slot->next;
        if (slot->drop) slot->drop(slot->value);
        free(slot);
        slot = next;
    }
    free(context);
}

/* Explicit pthread cleanup means a guest/FFI pthread_exit or a deferred
 * cancellation still reclaims the captured argument and physical TLS values.
 * Clearing the TLS key BEFORE running drops prevents reentrant callbacks
 * from resurrecting a context whose lifetime has ended. */
static void ores_thread_cleanup_v1(void *opaque) {
    ores_start_v1 *start = (ores_start_v1 *)opaque;
    (void)pthread_setspecific(ores_tls_key_v1, NULL);
    if (start->drop) start->drop(start->arg);
    ores_context_destroy_v1(start->context);
    atomic_store_explicit(&start->handle->body_completed, 1, memory_order_release);
    free(start);
}

static void *ores_thread_trampoline_v1(void *opaque) {
    ores_start_v1 *start = (ores_start_v1 *)opaque;
    ores_native_thread_v1 *handle = start->handle;
    if (pthread_setspecific(ores_tls_key_v1, start->context) == 0) {
        /* The push/pop pair is deliberately in the same lexical scope. */
        pthread_cleanup_push(ores_thread_cleanup_v1, start);
        handle->result = start->entry(start->arg);
        atomic_store_explicit(&handle->abnormal_exit, 0, memory_order_release);
        pthread_cleanup_pop(1);
    } else {
        /* The thread never entered the guest callback, but still owns arg. */
        ores_thread_cleanup_v1(start);
    }
    return NULL;
}

ores_thread_status_v1 ores_native_thread_spawn_v1(
    ores_native_thread_entry_v1 entry, void *owned_arg,
    ores_native_thread_drop_v1 drop_arg, ores_native_thread_v1 **out) {
    if (!entry || !out || (owned_arg && !drop_arg)) return ORES_THREAD_BAD_ARGUMENT_V1;
    *out = NULL;
    if (pthread_once(&ores_tls_once_v1, ores_tls_init_v1) != 0 ||
        atomic_load(&ores_tls_result_v1) != 0) return ORES_THREAD_OS_ERROR_V1;
    ores_native_thread_v1 *handle = calloc(1, sizeof(*handle));
    ores_context_v1 *context = calloc(1, sizeof(*context));
    ores_start_v1 *start = calloc(1, sizeof(*start));
    if (!handle || !context || !start) {
        free(handle); free(context); free(start);
        return ORES_THREAD_OUT_OF_MEMORY_V1;
    }
    context->id = ores_allocate_thread_id_v1();
    if (!context->id) {
        free(handle); free(context); free(start);
        return ORES_THREAD_OS_ERROR_V1;
    }
    atomic_init(&handle->join_state, 0);
    atomic_init(&handle->body_completed, 0);
    atomic_init(&handle->abnormal_exit, 1);
    start->entry = entry; start->arg = owned_arg; start->drop = drop_arg;
    start->context = context; start->handle = handle;
    if (pthread_create(&handle->native, NULL, ores_thread_trampoline_v1, start) != 0) {
        free(handle); free(context); free(start);
        return ORES_THREAD_OS_ERROR_V1; /* ownership NOT transferred */
    }
    *out = handle;
    return ORES_THREAD_OK_V1;
}

ores_thread_status_v1 ores_native_thread_join_blocking_v1(
    ores_native_thread_v1 *handle, int *out_result) {
    if (!handle || !out_result) return ORES_THREAD_BAD_ARGUMENT_V1;
    if (pthread_equal(pthread_self(), handle->native)) return ORES_THREAD_SELF_JOIN_V1;
    int expected = 0;
    if (!atomic_compare_exchange_strong(&handle->join_state, &expected, 1))
        return ORES_THREAD_ALREADY_JOINED_V1;
    if (pthread_join(handle->native, NULL) != 0) {
        atomic_store(&handle->join_state, 0);
        return ORES_THREAD_OS_ERROR_V1;
    }
    const int abnormal = atomic_load_explicit(
        &handle->abnormal_exit, memory_order_acquire);
    if (!abnormal) *out_result = handle->result;
    atomic_store(&handle->join_state, 2);
    return abnormal ? ORES_THREAD_UNEXPECTED_EXIT_V1 : ORES_THREAD_OK_V1;
}

ores_thread_status_v1 ores_native_thread_release_v1(
    ores_native_thread_v1 *handle) {
    if (!handle) return ORES_THREAD_BAD_ARGUMENT_V1;
    if (atomic_load(&handle->join_state) != 2) return ORES_THREAD_ALREADY_JOINED_V1;
    /* Only a physically joined handle may retire a scope reservation.
     * This deliberately does NOT release quota at body completion. */
    if (handle->scope) {
        ores_native_thread_scope_v1 *scope = handle->scope;
        if (pthread_mutex_lock(&scope->lock) != 0) return ORES_THREAD_OS_ERROR_V1;
        if (!scope->outstanding) {
            (void)pthread_mutex_unlock(&scope->lock);
            return ORES_THREAD_OS_ERROR_V1;
        }
        --scope->outstanding;
        if (pthread_mutex_unlock(&scope->lock) != 0) return ORES_THREAD_OS_ERROR_V1;
    }
    free(handle); /* caller must hold exclusive ownership of handle */
    return ORES_THREAD_OK_V1;
}

/* These functions are host/runtime isolate capabilities, not guest FFI.
 * A caller must guarantee that a scope is not destroyed concurrently with
 * an attempted operation that has not yet acquired its scope mutex. */
ores_thread_status_v1 ores_native_thread_scope_open_v1(
    unsigned capacity, ores_native_thread_scope_v1 **out) {
    if (!out) return ORES_THREAD_BAD_ARGUMENT_V1;
    *out = NULL;
    if (capacity == 0 || capacity > 1024) return ORES_THREAD_BAD_ARGUMENT_V1;
    ores_native_thread_scope_v1 *scope = calloc(1, sizeof(*scope));
    if (!scope) return ORES_THREAD_OUT_OF_MEMORY_V1;
    if (pthread_mutex_init(&scope->lock, NULL) != 0) {
        free(scope);
        return ORES_THREAD_OS_ERROR_V1;
    }
    scope->capacity = capacity;
    *out = scope;
    return ORES_THREAD_OK_V1;
}

ores_thread_status_v1 ores_native_thread_scope_spawn_v1(
    ores_native_thread_scope_v1 *scope,
    ores_native_thread_entry_v1 entry, void *owned_arg,
    ores_native_thread_drop_v1 drop_arg, ores_native_thread_v1 **out) {
    if (!out) return ORES_THREAD_BAD_ARGUMENT_V1;
    *out = NULL;
    if (!scope || !entry || (owned_arg && !drop_arg))
        return ORES_THREAD_BAD_ARGUMENT_V1;
    if (pthread_mutex_lock(&scope->lock) != 0) return ORES_THREAD_OS_ERROR_V1;
    ores_thread_status_v1 denied = ORES_THREAD_OK_V1;
    if (scope->closing) denied = ORES_THREAD_SCOPE_CLOSED_V1;
    else if (scope->outstanding >= scope->capacity)
        denied = ORES_THREAD_SCOPE_FULL_V1;
    else ++scope->outstanding; /* reserve BEFORE possible pthread_create */
    if (pthread_mutex_unlock(&scope->lock) != 0) return ORES_THREAD_OS_ERROR_V1;
    if (denied != ORES_THREAD_OK_V1) return denied;

    ores_native_thread_v1 *handle = NULL;
    const ores_thread_status_v1 result =
        ores_native_thread_spawn_v1(entry, owned_arg, drop_arg, &handle);
    if (result != ORES_THREAD_OK_V1) {
        /* No argument transfer on failure. The previously reserved quota
         * remains inaccessible to close/destroy until this release. */
        if (pthread_mutex_lock(&scope->lock) != 0) abort();
        if (!scope->outstanding) abort();
        --scope->outstanding;
        if (pthread_mutex_unlock(&scope->lock) != 0) abort();
        return result;
    }
    handle->scope = scope; /* no other caller owns the handle yet */
    *out = handle;
    return ORES_THREAD_OK_V1;
}

ores_thread_status_v1 ores_native_thread_scope_outstanding_v1(
    ores_native_thread_scope_v1 *scope, unsigned *out) {
    if (!scope || !out) return ORES_THREAD_BAD_ARGUMENT_V1;
    if (pthread_mutex_lock(&scope->lock) != 0) return ORES_THREAD_OS_ERROR_V1;
    *out = scope->outstanding;
    if (pthread_mutex_unlock(&scope->lock) != 0) return ORES_THREAD_OS_ERROR_V1;
    return ORES_THREAD_OK_V1;
}

ores_thread_status_v1 ores_native_thread_scope_close_v1(
    ores_native_thread_scope_v1 *scope) {
    if (!scope) return ORES_THREAD_BAD_ARGUMENT_V1;
    if (pthread_mutex_lock(&scope->lock) != 0) return ORES_THREAD_OS_ERROR_V1;
    scope->closing = 1; /* permanently revoke admission */
    const int busy = scope->outstanding != 0;
    if (pthread_mutex_unlock(&scope->lock) != 0) return ORES_THREAD_OS_ERROR_V1;
    return busy ? ORES_THREAD_SCOPE_BUSY_V1 : ORES_THREAD_OK_V1;
}

ores_thread_status_v1 ores_native_thread_scope_destroy_v1(
    ores_native_thread_scope_v1 *scope) {
    if (!scope) return ORES_THREAD_BAD_ARGUMENT_V1;
    if (pthread_mutex_lock(&scope->lock) != 0) return ORES_THREAD_OS_ERROR_V1;
    const int busy = !scope->closing || scope->outstanding != 0;
    if (pthread_mutex_unlock(&scope->lock) != 0) return ORES_THREAD_OS_ERROR_V1;
    if (busy) return ORES_THREAD_SCOPE_BUSY_V1;
    /* Requires external supervision fencing all outstanding API callers. */
    if (pthread_mutex_destroy(&scope->lock) != 0) return ORES_THREAD_OS_ERROR_V1;
    free(scope);
    return ORES_THREAD_OK_V1;
}

/* Isolate identity guards are defense-in-depth routing checks.
 * Identity is NOT a bearer capability: only a trusted supervisor may hold
 * the scope pointer and mint the expected key passed to these entry points.
 * The original unkeyed host functions remain internal/privileged ABI. */
static int ores_isolate_key_valid_v1(ores_native_thread_isolate_key_v1 key) {
    return key.tenant_id != 0 && key.isolate_id != 0 &&
           key.generation != 0;
}

static int ores_isolate_key_matches_v1(
    const ores_native_thread_scope_v1 *scope,
    ores_native_thread_isolate_key_v1 expected) {
    return scope->isolate_key.tenant_id == expected.tenant_id &&
           scope->isolate_key.isolate_id == expected.isolate_id &&
           scope->isolate_key.generation == expected.generation;
}

ores_thread_status_v1 ores_native_thread_scope_open_isolate_v1(
    ores_native_thread_isolate_key_v1 key, unsigned capacity,
    ores_native_thread_scope_v1 **out) {
    if (!out) return ORES_THREAD_BAD_ARGUMENT_V1;
    *out = NULL;
    if (!ores_isolate_key_valid_v1(key))
        return ORES_THREAD_BAD_ARGUMENT_V1;
    ores_native_thread_scope_v1 *scope = NULL;
    const ores_thread_status_v1 result =
        ores_native_thread_scope_open_v1(capacity, &scope);
    if (result != ORES_THREAD_OK_V1) return result;
    scope->isolate_key = key; /* caller has not yet received the pointer */
    *out = scope;
    return ORES_THREAD_OK_V1;
}

ores_thread_status_v1 ores_native_thread_scope_spawn_isolate_v1(
    ores_native_thread_scope_v1 *scope,
    ores_native_thread_isolate_key_v1 expected,
    ores_native_thread_entry_v1 entry, void *owned_arg,
    ores_native_thread_drop_v1 drop_arg, ores_native_thread_v1 **out) {
    if (!out) return ORES_THREAD_BAD_ARGUMENT_V1;
    *out = NULL;
    if (!scope || !ores_isolate_key_valid_v1(expected) ||
            !entry || (owned_arg && !drop_arg))
        return ORES_THREAD_BAD_ARGUMENT_V1;
    if (!ores_isolate_key_matches_v1(scope, expected))
        return ORES_THREAD_SCOPE_IDENTITY_MISMATCH_V1;
    return ores_native_thread_scope_spawn_v1(
        scope, entry, owned_arg, drop_arg, out);
}

ores_thread_status_v1 ores_native_thread_scope_close_isolate_v1(
    ores_native_thread_scope_v1 *scope,
    ores_native_thread_isolate_key_v1 expected) {
    if (!scope || !ores_isolate_key_valid_v1(expected))
        return ORES_THREAD_BAD_ARGUMENT_V1;
    if (!ores_isolate_key_matches_v1(scope, expected))
        return ORES_THREAD_SCOPE_IDENTITY_MISMATCH_V1;
    return ores_native_thread_scope_close_v1(scope);
}

ores_thread_status_v1 ores_native_thread_scope_destroy_isolate_v1(
    ores_native_thread_scope_v1 *scope,
    ores_native_thread_isolate_key_v1 expected) {
    if (!scope || !ores_isolate_key_valid_v1(expected))
        return ORES_THREAD_BAD_ARGUMENT_V1;
    if (!ores_isolate_key_matches_v1(scope, expected))
        return ORES_THREAD_SCOPE_IDENTITY_MISMATCH_V1;
    return ores_native_thread_scope_destroy_v1(scope);
}

ores_thread_status_v1 ores_native_thread_id_v1(uint64_t *out_id) {
    if (!out_id) return ORES_THREAD_BAD_ARGUMENT_V1;
    ores_context_v1 *context = ores_context_current_v1();
    if (!context) return ORES_THREAD_NO_CONTEXT_V1;
    *out_id = context->id;
    return ORES_THREAD_OK_V1;
}

ores_thread_status_v1 ores_native_thread_local_get_v1(
    uint64_t key, void **borrowed_value) {
    if (!key || !borrowed_value) return ORES_THREAD_BAD_ARGUMENT_V1;
    *borrowed_value = NULL;
    ores_context_v1 *context = ores_context_current_v1();
    if (!context) return ORES_THREAD_NO_CONTEXT_V1;
    for (ores_slot_v1 *slot = context->slots; slot; slot = slot->next)
        if (slot->key == key) {
            *borrowed_value = slot->value;
            break;
        }
    return ORES_THREAD_OK_V1;
}

ores_thread_status_v1 ores_native_thread_local_set_v1(
    uint64_t key, void *owned_value, ores_native_thread_drop_v1 drop) {
    if (!key || (owned_value && !drop)) return ORES_THREAD_BAD_ARGUMENT_V1;
    ores_context_v1 *context = ores_context_current_v1();
    if (!context) return ORES_THREAD_NO_CONTEXT_V1;
    for (ores_slot_v1 *slot = context->slots; slot; slot = slot->next) {
        if (slot->key == key) {
            if (owned_value == slot->value) return ORES_THREAD_BAD_ARGUMENT_V1;
            void *old = slot->value;
            ores_native_thread_drop_v1 old_drop = slot->drop;
            slot->value = owned_value; slot->drop = drop;
            if (old_drop) old_drop(old);
            return ORES_THREAD_OK_V1;
        }
    }
    if (context->slots_count >= ORES_THREAD_MAX_SLOTS_V1)
        return ORES_THREAD_SLOT_LIMIT_V1;
    ores_slot_v1 *slot = malloc(sizeof(*slot));
    if (!slot) return ORES_THREAD_OUT_OF_MEMORY_V1;
    slot->key = key; slot->value = owned_value; slot->drop = drop;
    slot->next = context->slots;
    context->slots = slot;
    ++context->slots_count;
    return ORES_THREAD_OK_V1;
}

ores_thread_status_v1 ores_native_thread_local_take_v1(
    uint64_t key, void **owned_value, ores_native_thread_drop_v1 *drop) {
    if (!key || !owned_value || !drop) return ORES_THREAD_BAD_ARGUMENT_V1;
    *owned_value = NULL; *drop = NULL;
    ores_context_v1 *context = ores_context_current_v1();
    if (!context) return ORES_THREAD_NO_CONTEXT_V1;
    ores_slot_v1 **link = &context->slots;
    while (*link) {
        ores_slot_v1 *slot = *link;
        if (slot->key == key) {
            *owned_value = slot->value; *drop = slot->drop;
            *link = slot->next;
            free(slot); --context->slots_count;
            return ORES_THREAD_OK_V1;
        }
        link = &slot->next;
    }
    return ORES_THREAD_OK_V1;
}

/* This is a release/acquire completion notification, NOT an OS-thread join.
 * Caller must own/retain handle through access. Physical join is performed
 * only by the structured host reaper. */
ores_thread_status_v1 ores_native_thread_poll_completed_v1(
    const ores_native_thread_v1 *handle, int *out_completed) {
    if (!handle || !out_completed) return ORES_THREAD_BAD_ARGUMENT_V1;
    *out_completed = atomic_load_explicit(
        &handle->body_completed, memory_order_acquire);
    return ORES_THREAD_OK_V1;
}
