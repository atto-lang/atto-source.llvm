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

struct ores_native_thread_v1 {
    pthread_t native;
    atomic_int join_state; /* 0 joinable, 1 joining, 2 joined */
    atomic_int body_completed; /* release after all TLS destructors */
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
static atomic_int ores_tls_result_v1 = ATOMIC_VAR_INIT(0);
static atomic_uint_fast64_t ores_id_counter_v1 = ATOMIC_VAR_INIT(1);

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

static void *ores_thread_trampoline_v1(void *opaque) {
    ores_start_v1 *start = (ores_start_v1 *)opaque;
    ores_context_v1 *context = start->context;
    ores_native_thread_v1 *handle = start->handle;
    if (pthread_setspecific(ores_tls_key_v1, context) == 0) {
        handle->result = start->entry(start->arg);
        (void)pthread_setspecific(ores_tls_key_v1, NULL);
    } else {
        handle->result = -1; /* OS TLS admission failed; entry never ran */
    }
    if (start->drop) start->drop(start->arg);
    ores_context_destroy_v1(context); /* all TLS values dropped before join */
    atomic_store_explicit(&handle->body_completed, 1, memory_order_release);
    free(start);
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
    context->id = atomic_fetch_add(&ores_id_counter_v1, 1);
    if (!context->id || context->id == UINT64_MAX) {
        free(handle); free(context); free(start);
        return ORES_THREAD_OS_ERROR_V1;
    }
    atomic_init(&handle->join_state, 0);
    atomic_init(&handle->body_completed, 0);
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
    *out_result = handle->result;
    atomic_store(&handle->join_state, 2);
    return ORES_THREAD_OK_V1;
}

ores_thread_status_v1 ores_native_thread_release_v1(
    ores_native_thread_v1 *handle) {
    if (!handle) return ORES_THREAD_BAD_ARGUMENT_V1;
    if (atomic_load(&handle->join_state) != 2) return ORES_THREAD_ALREADY_JOINED_V1;
    free(handle); /* caller must hold exclusive ownership of handle */
    return ORES_THREAD_OK_V1;
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
