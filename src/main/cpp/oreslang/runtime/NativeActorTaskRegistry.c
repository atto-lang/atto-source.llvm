#include "oreslang/runtime/NativeActorTaskRegistry.h"
#include <stdint.h>
#include <string.h>

static int same_actor(OresNativeActorRef a, OresNativeActorRef b) {
    return a.scheduler_domain == b.scheduler_domain
        && a.actor_slot == b.actor_slot && a.generation == b.generation;
}
static size_t lookup(const OresNativeActorTaskRegistry* r,
        const OresNativeActorTask* task) {
    for (size_t i = 0; i < r->count; ++i)
        if (r->entries[i].task == task) return i;
    return r->count;
}
int ores_native_actor_task_registry_init(
        OresNativeActorTaskRegistry* r, OresNativeActorScheduler* s,
        OresNativeActorRef actor, OresNativeActorTaskEntry* entries,
        size_t capacity) {
    if (!r || !s || !entries || !capacity
            || capacity > ORES_NATIVE_WAKE_CREDITS_PER_ACTOR
            || !actor.scheduler_domain || !actor.generation
            || !ores_native_actor_scheduler_poll_ready(s, actor))
        return 0;
    memset(r, 0, sizeof(*r));
    if (pthread_mutex_init(&r->mutex, NULL) != 0) return 0;
    memset(entries, 0, capacity * sizeof(*entries));
    r->scheduler = s;
    r->actor = actor;
    r->entries = entries;
    r->capacity = capacity;
    r->initialized = 1;
    return 1;
}
int ores_native_actor_task_registry_start(
        OresNativeActorTaskRegistry* r, OresNativeActorTask* task,
        OresNativeActorTask* parent) {
    if (!r || !r->initialized || !task) return 0;
    pthread_mutex_lock(&r->mutex);
    int ok = !r->sealed && r->count < r->capacity
        && lookup(r, task) == r->count
        && task->initialized && task->scheduler == r->scheduler
        && same_actor(task->actor, r->actor);
    if (ok && parent) {
        ok = parent != task
            && lookup(r, parent) != r->count
            && ores_native_actor_task_is_current_running(parent);
    }
    if (ok) {
        /* The Task bridge retains its physical actor loan and admits its
         * initial WAKE before this entry becomes visible. The actor's own
         * single-writer DATA/WAKE claim prevents an on_wake callback from
         * racing this insertion. STOP serializes on the same registry mutex. */
        ok = ores_native_actor_task_start(task);
        if (ok)
            r->entries[r->count++] = (OresNativeActorTaskEntry){task, parent};
    }
    pthread_mutex_unlock(&r->mutex);
    return ok;
}
int ores_native_actor_task_registry_stop(OresNativeActorTaskRegistry* r) {
    if (!r || !r->initialized) return 0;
    pthread_mutex_lock(&r->mutex);
    if (r->sealed) {
        pthread_mutex_unlock(&r->mutex);
        return 0;
    }
    r->sealed = 1;
    /* Each canceled suspended Task releases a transferred wake credit. A
     * running Task is canceled cooperatively and keeps its resource loan
     * until the after-WAKE physical exit receipt; STOP cannot forge DONE. */
    for (size_t i = 0; i < r->count; ++i)
        (void)ores_native_actor_task_cancel(r->entries[i].task);
    int stopped = ores_native_actor_scheduler_stop_sealed(
        r->scheduler, r->actor);
    pthread_mutex_unlock(&r->mutex);
    return stopped;
}
int ores_native_actor_task_registry_cancel(
        OresNativeActorTaskRegistry* r, OresNativeActorTask* anchor) {
    if (!r || !r->initialized || !anchor) return 0;
    pthread_mutex_lock(&r->mutex);
    int found = lookup(r, anchor) < r->count;
    if (found) {
        /* Snapshot not needed: child admission is serialized under this
         * same lock, including parent execution and STOP. Cancel every
         * registered transitive child even if its parent result is terminal.
         * A foreign Future producer cannot execute the canceled guest. */
        for (size_t i = 0; i < r->count; ++i) {
            OresNativeActorTask* cursor = r->entries[i].task;
            size_t depth = 0;
            while (cursor && depth++ <= r->count) {
                if (cursor == anchor) {
                    (void)ores_native_actor_task_cancel(r->entries[i].task);
                    break;
                }
                size_t parent = lookup(r, cursor);
                if (parent == r->count) break;
                cursor = r->entries[parent].parent;
            }
        }
    }
    pthread_mutex_unlock(&r->mutex);
    return found;
}
int ores_native_actor_task_registry_scope_done(
        OresNativeActorTaskRegistry* r, OresNativeActorTask* anchor) {
    if (!r || !r->initialized || !anchor) return 0;
    pthread_mutex_lock(&r->mutex);
    const size_t owner = lookup(r, anchor);
    int done = owner != r->count
        && ores_native_actor_task_physically_done(anchor);
    /* All parent pointers remain valid while registered. A parent may be
     * terminal itself but still has a suspended child. Do not announce
     * TaskHandle DONE or release its owned resources prematurely. */
    for (size_t i = 0; done && i < r->count; ++i) {
        OresNativeActorTask* cursor = r->entries[i].parent;
        size_t depth = 0;
        while (cursor && depth++ < r->count) {
            if (cursor == anchor) {
                if (!ores_native_actor_task_physically_done(
                        r->entries[i].task)) done = 0;
                break;
            }
            size_t ancestor = lookup(r, cursor);
            if (ancestor == r->count) { done = 0; break; }
            cursor = r->entries[ancestor].parent;
        }
        if (cursor && depth > r->count) done = 0; /* never trust a cycle */
    }
    pthread_mutex_unlock(&r->mutex);
    return done;
}
int ores_native_actor_task_registry_unregister(
        OresNativeActorTaskRegistry* r, OresNativeActorTask* task) {
    if (!r || !r->initialized || !task) return 0;
    pthread_mutex_lock(&r->mutex);
    size_t i = lookup(r, task);
    int ok = i < r->count && ores_native_actor_task_physically_done(task);
    /* Protect descendant chain even when its parent has physically exited:
     * the parent record must live until every direct child was unregistered. */
    for (size_t j = 0; ok && j < r->count; ++j)
        if (r->entries[j].parent == task) ok = 0;
    if (ok) {
        r->entries[i] = r->entries[--r->count];
        r->entries[r->count] = (OresNativeActorTaskEntry){0};
    }
    pthread_mutex_unlock(&r->mutex);
    return ok;
}
size_t ores_native_actor_task_registry_count(
        OresNativeActorTaskRegistry* r) {
    if (!r || !r->initialized) return 0;
    pthread_mutex_lock(&r->mutex);
    size_t count = r->count;
    pthread_mutex_unlock(&r->mutex);
    return count;
}
int ores_native_actor_task_registry_close(
        OresNativeActorTaskRegistry* r) {
    if (!r || !r->initialized) return 0;
    pthread_mutex_lock(&r->mutex);
    /* Closing an unsealed actor registry could silently disable automatic
     * Task tracking while the actor is still accepting guest calls. */
    int ok = r->sealed && r->count == 0;
    if (ok) r->initialized = 0;
    pthread_mutex_unlock(&r->mutex);
    if (ok && pthread_mutex_destroy(&r->mutex) != 0) return 0;
    return ok;
}
