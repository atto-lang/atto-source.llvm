#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "oreslang/runtime/NativeOresVMNio.h"
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define check(valid) do { if (!(valid)) { \
    fprintf(stderr, "NIO lifecycle check failed at %s:%d: %s\n", __FILE__, __LINE__, #valid); \
    abort(); \
} } while (0)

typedef struct {
    pid_t ids[128];
    size_t count;
} ThreadIdSet;

static int compare_tid(const void* left, const void* right) {
    const pid_t a = *(const pid_t*)left;
    const pid_t b = *(const pid_t*)right;
    return (a > b) - (a < b);
}

static ThreadIdSet snapshot_thread_ids(void) {
    ThreadIdSet set = {{0}, 0};
    DIR* tasks = opendir("/proc/self/task");
    check(tasks != NULL);
    struct dirent* entry;
    while ((entry = readdir(tasks)) != NULL) {
        char* end = NULL;
        const long value = strtol(entry->d_name, &end, 10);
        if (end == entry->d_name || *end != '\0' || value <= 0) continue;
        check(set.count < sizeof(set.ids) / sizeof(set.ids[0]));
        set.ids[set.count++] = (pid_t)value;
    }
    check(closedir(tasks) == 0);
    qsort(set.ids, set.count, sizeof(set.ids[0]), compare_tid);
    return set;
}

/* pthread_join can finish before procfs stops listing the exiting worker
 * from the previous VM. Establish the exact expected RT thread census during
 * bootstrap; never normalize/retry the post-operation identity assertion. */
static ThreadIdSet await_thread_count(size_t expected) {
    const struct timespec pause = {0, 1000000};
    for (int attempt = 0; attempt < 3000; ++attempt) {
        const ThreadIdSet set = snapshot_thread_ids();
        if (set.count == expected) return set;
        nanosleep(&pause, NULL);
    }
    check(0 && "RT bootstrap thread census did not settle");
    return (ThreadIdSet){{0}, 0};
}

static void check_same_thread_ids(ThreadIdSet before, ThreadIdSet after) {
    if (before.count != after.count) {
        fprintf(stderr, "thread census before(%zu):", before.count);
        for (size_t i = 0; i < before.count; ++i) fprintf(stderr, " %ld", (long)before.ids[i]);
        fprintf(stderr, "\nafter(%zu):", after.count);
        for (size_t i = 0; i < after.count; ++i) fprintf(stderr, " %ld", (long)after.ids[i]);
        fprintf(stderr, "\n");
    }
    check(before.count == after.count);
    for (size_t i = 0; i < before.count; ++i)
        check(before.ids[i] == after.ids[i]);
}

typedef struct {
    OresNativeVM* vm;
    atomic_int entered;
    atomic_int finished;
} NioCloseRace;
static void* foreign_abort_bootstrap(void* opaque) {
    ores_native_vm_nio_cancel_bootstrap((OresNativeVM*)opaque);
    return NULL;
}
static void* close_while_leased(void* opaque) {
    NioCloseRace* race = (NioCloseRace*)opaque;
    atomic_store(&race->entered, 1);
    check(ores_native_vm_close(race->vm));
    atomic_store(&race->finished, 1);
    return NULL;
}
/* Do not guess with a 10ms sleep that another pthread has reached VM
 * retirement. While the test owns a backend/bootstrap pin, close cannot free
 * the VM, so a bounded trusted-host CONTROL probe is safe and deterministic.
 */
static void await_retirement(OresNativeVM* vm) {
    struct timespec pause = {0, 1000000};
    for (int attempt = 0; attempt < 3000; ++attempt) {
        if (!ores_native_vm_control_notify(vm, ORES_NATIVE_VM_MAIN)) return;
        nanosleep(&pause, NULL);
    }
    check(0 && "timed out waiting for lifecycle transition");
}
static void on_data(void* ignored, size_t slot, uint64_t value) {
    (void)ignored; (void)slot; (void)value;
    abort(); /* no guest callbacks may be delivered on NIO-only test path */
}
typedef struct {
    OresNativeVM* vm;
    int fd;
    atomic_int attempted;
    atomic_int bare_watch_result;
} ActorNioProbe;

typedef struct {
    OresNativeVM* vm;
    enum OresNativeVMDomain domain;
    OresNativeActorRef actor;
    int fd;
    atomic_int entered;
    atomic_int go;
    atomic_int attempted;
    atomic_int accepted;
    OresNativeActorNioWatch watch;
} OwnedActorNioProbe;

typedef struct {
    OresNativeVM* vm;
    enum OresNativeVMDomain domain;
    OresNativeActorRef actor;
    int fd;
    atomic_int active;
    atomic_int entered;
    atomic_int go;
    atomic_int attempted;
    atomic_int accepted;
    atomic_int duplicate_rejected;
    atomic_int wakes;
    atomic_int wake_outcome;
    atomic_int wake_errors;
    atomic_int rearm_attempted;
    atomic_int rearm_accepted;
    OresNativeActorNioWatch rearm_watch;
    OresNativeActorNioAwait await;
} OwnedActorNioAwaitProbe;

static int same_actor_ref(OresNativeActorRef left, OresNativeActorRef right);

static void on_actor_nio_probe(void* opaque, size_t slot, uint64_t value) {
    ActorNioProbe* probe = (ActorNioProbe*)opaque;
    check(probe != NULL && probe->vm != NULL && probe->fd >= 0);
    check(slot == 0 && value == UINT64_C(77));
    OresNativeNioWatch watch = {0};
    const int accepted = ores_native_vm_nio_try_watch(
        probe->vm, probe->fd, EPOLLIN, &watch);
    atomic_store_explicit(
        &probe->bare_watch_result, accepted, memory_order_release);
    atomic_store_explicit(&probe->attempted, 1, memory_order_release);
}

static void on_owned_actor_nio(void* opaque, size_t slot, uint64_t value) {
    OwnedActorNioProbe* probe = (OwnedActorNioProbe*)opaque;
    check(probe != NULL && probe->vm != NULL && probe->fd >= 0);
    check(slot == probe->actor.actor_slot && value == UINT64_C(88));
    atomic_store_explicit(&probe->entered, 1, memory_order_release);
    while (!atomic_load_explicit(&probe->go, memory_order_acquire))
        sched_yield();

    OresNativeActorNioWatch watch = {0};
    OresNativeActorRef wrong = probe->actor;
    ++wrong.generation;
    check(!ores_native_vm_nio_try_watch_actor(
        probe->vm, probe->domain, wrong,
        probe->fd, EPOLLIN, &watch));
    const int accepted = ores_native_vm_nio_try_watch_actor(
        probe->vm, probe->domain, probe->actor,
        probe->fd, EPOLLIN, &watch);
    if (accepted) probe->watch = watch;
    atomic_store_explicit(&probe->accepted, accepted, memory_order_release);
    atomic_store_explicit(&probe->attempted, 1, memory_order_release);
}

static int same_wake_ticket(
        OresNativeWakeTicket left, OresNativeWakeTicket right) {
    return same_actor_ref(left.producer, right.producer)
        && same_actor_ref(left.target, right.target)
        && left.slot == right.slot
        && left.slot_generation == right.slot_generation
        && left.continuation_slot == right.continuation_slot
        && left.continuation_generation == right.continuation_generation
        && left.code_generation == right.code_generation
        && left.descriptor_generation == right.descriptor_generation;
}

static void on_owned_actor_nio_await(
        void* opaque, size_t slot, uint64_t value) {
    OwnedActorNioAwaitProbe* probe = (OwnedActorNioAwaitProbe*)opaque;
    check(probe != NULL && probe->vm != NULL && probe->fd >= 0);
    check(slot == probe->actor.actor_slot && value == UINT64_C(99));
    check(atomic_fetch_add_explicit(
        &probe->active, 1, memory_order_acq_rel) == 0);
    atomic_store_explicit(&probe->entered, 1, memory_order_release);
    while (!atomic_load_explicit(&probe->go, memory_order_acquire))
        sched_yield();

    OresNativeActorNioAwait awaiting = {0};
    const int accepted = ores_native_vm_nio_try_watch_actor_await(
        probe->vm, probe->domain, probe->actor,
        probe->fd, EPOLLIN,
        UINT64_C(101), UINT64_C(5), UINT64_C(7), UINT64_C(11),
        &awaiting);
    if (accepted) {
        probe->await = awaiting;

        // A second suspension on the same FD must reject and refund its
        // speculative wake/resource loans, leaving exactly the first await.
        OresNativeActorNioAwait duplicate = {0};
        const int duplicate_accepted = ores_native_vm_nio_try_watch_actor_await(
            probe->vm, probe->domain, probe->actor,
            probe->fd, EPOLLIN,
            UINT64_C(102), UINT64_C(6), UINT64_C(7), UINT64_C(12),
            &duplicate);
        atomic_store_explicit(
            &probe->duplicate_rejected, !duplicate_accepted,
            memory_order_release);
    }
    atomic_store_explicit(&probe->accepted, accepted, memory_order_release);
    atomic_store_explicit(&probe->attempted, 1, memory_order_release);
    check(atomic_fetch_sub_explicit(
        &probe->active, 1, memory_order_acq_rel) == 1);
}

static void on_owned_actor_nio_wake(
        void* opaque, size_t slot, OresNativeWakeTicket ticket,
        OresNativeWakeOutcome outcome) {
    OwnedActorNioAwaitProbe* probe = (OwnedActorNioAwaitProbe*)opaque;
    int errors = 0;
    if (slot != probe->actor.actor_slot
            || !ores_native_vm_actor_is_current_wake(
                probe->vm, probe->domain, ticket)
            || !same_wake_ticket(ticket, probe->await.wake)
            || atomic_fetch_add_explicit(
                &probe->active, 1, memory_order_acq_rel) != 0)
        ++errors;

    OresNativeWakeTicket forged = ticket;
    ++forged.continuation_generation;
    if (ores_native_vm_actor_is_current_wake(
            probe->vm, probe->domain, forged))
        ++errors;

    if (outcome == ORES_NATIVE_WAKE_READY) {
        OresNativeActorNioWatch rearm = {0};
        const int accepted = ores_native_vm_nio_try_watch_actor(
            probe->vm, probe->domain, probe->actor,
            probe->fd, EPOLLIN, &rearm);
        if (accepted) probe->rearm_watch = rearm;
        atomic_store_explicit(
            &probe->rearm_accepted, accepted, memory_order_release);
        atomic_store_explicit(
            &probe->rearm_attempted, 1, memory_order_release);
    }

    atomic_store_explicit(
        &probe->wake_outcome, (int)outcome, memory_order_release);
    atomic_fetch_add_explicit(&probe->wakes, 1, memory_order_acq_rel);
    if (errors)
        atomic_fetch_add_explicit(
            &probe->wake_errors, errors, memory_order_acq_rel);
    check(atomic_fetch_sub_explicit(
        &probe->active, 1, memory_order_acq_rel) == 1);
}

static int same_actor_ref(OresNativeActorRef left, OresNativeActorRef right) {
    return left.scheduler_domain == right.scheduler_domain
        && left.actor_slot == right.actor_slot
        && left.generation == right.generation;
}

static OresNativeActorNioCompletion await_actor_completion(OresNativeVM* vm) {
    OresNativeActorNioCompletion completed = {0};
    struct timespec pause = {0, 1000000};
    for (int i = 0; i < 3000; ++i) {
        if (ores_native_vm_nio_try_pop_actor(vm, &completed)) return completed;
        nanosleep(&pause, NULL);
    }
    check(0 && "timed out waiting for actor NIO completion");
    return completed;
}

static OresNativeVMConfig config(void) {
    OresNativeVMConfig cfg = {0};
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i) {
        cfg.domains[i].actor_slots = 2;
        cfg.domains[i].mailbox_capacity = 4;
        cfg.domains[i].carrier_workers = 2;
        cfg.domains[i].on_message = on_data;
    }
    return cfg;
}
static OresNativeNioCompletion await_completion(OresNativeVM* vm) {
    OresNativeNioCompletion completed = {0};
    struct timespec pause = {0, 1000000};
    for (int i = 0; i < 3000; ++i) {
        if (ores_native_vm_nio_try_pop(vm, &completed)) return completed;
        nanosleep(&pause, NULL); /* test harness only; never actor/CONTROL */
    }
    check(0 && "timed out waiting for lifecycle transition");
}
int main(void) {
    OresNativeVMConfig cfg = config();
    OresNativeVM* vm = ores_native_vm_open(&cfg);
    check(vm != NULL);
    check(ores_native_vm_nio_started(vm) == 0);
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i)
        check(ores_native_vm_started_carriers(vm, (enum OresNativeVMDomain)i) == 0);
    check(ores_native_vm_enable_nio(vm, 4, 0, 1));
    check(!ores_native_vm_enable_nio(vm, 4, 0, 1));
    check(ores_native_vm_nio_started(vm) == 0);
    int pipe_fd[2] = {-1, -1};
    check(pipe2(pipe_fd, O_NONBLOCK | O_CLOEXEC) == 0);
    OresNativeNioWatch watch = {0};
    check(ores_native_vm_nio_try_watch(vm, pipe_fd[0], EPOLLIN, &watch));
    check(ores_native_vm_nio_started(vm) == 1);
    check(!ores_native_vm_nio_try_watch(vm, pipe_fd[0], EPOLLIN, &watch));
    check(ores_native_vm_nio_completion_fd(vm, 0) >= 0);
    check(write(pipe_fd[1], "x", 1) == 1);
    const OresNativeNioCompletion done = await_completion(vm);
    check(done.outcome == ORES_NATIVE_NIO_READY);
    check(done.fd == pipe_fd[0] && (done.events & EPOLLIN) != 0);
    ores_native_vm_nio_drain_signal(vm, 0);
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i)
        check(ores_native_vm_started_carriers(vm, (enum OresNativeVMDomain)i) == 0);
    ores_native_vm_retire(vm);
    check(ores_native_vm_nio_started(vm) == 0);
    check(!ores_native_vm_nio_try_watch(vm, pipe_fd[0], EPOLLIN, &watch));
    check(!ores_native_vm_enable_nio(vm, 2, 0, 1));
    check(ores_native_vm_close(vm));
    close(pipe_fd[0]);
    close(pipe_fd[1]);

    // Bounded, caller-owned completion batches are a scheduler handoff
    // substrate, not an execution callback: five pending kernel operations
    // still use one epoll thread and no actor carrier is needed to drain.
    {
        OresNativeVM* batch_vm = ores_native_vm_open(&cfg);
        check(batch_vm != NULL);
        check(ores_native_vm_enable_nio(batch_vm, 1, 0, 1));
        check(ores_native_vm_nio_started(batch_vm) == 0);
        int pipes[5][2] = {{-1, -1}};
        OresNativeNioWatch tickets[5] = {{0}};
        for (size_t i = 0; i < 5; ++i) {
            check(pipe2(pipes[i], O_NONBLOCK | O_CLOEXEC) == 0);
            int admitted = 0;
            for (int retry = 0; retry < 2000 && !admitted; ++retry)
                admitted = ores_native_vm_nio_try_watch(
                    batch_vm, pipes[i][0], EPOLLIN, &tickets[i]);
            check(admitted);
        }
        check(ores_native_vm_nio_started(batch_vm) == 1);
        check(ores_native_vm_nio_try_pop_batch(batch_vm, NULL, 5) == 0);
        OresNativeNioCompletion completions[5] = {0};
        check(ores_native_vm_nio_try_pop_batch(batch_vm, completions, 0) == 0);
        for (size_t i = 0; i < 5; ++i)
            check(write(pipes[i][1], "x", 1) == 1);
        size_t consumed = 0;
        int seen[5] = {0};
        struct timespec pause = {0, 1000000};
        for (int retry = 0; retry < 3000 && consumed < 5; ++retry) {
            // Explicitly cap each host poll to 2 records. The runtime must
            // neither drop records nor exceed caller-provided output storage.
            const size_t room = (5 - consumed) < 2 ? (5 - consumed) : 2;
            const size_t got = ores_native_vm_nio_try_pop_batch(
                batch_vm, &completions[consumed], room);
            check(got <= room);
            for (size_t offset = 0; offset < got; ++offset) {
                const OresNativeNioCompletion* event =
                    &completions[consumed + offset];
                check(event->outcome == ORES_NATIVE_NIO_READY);
                int matched = 0;
                for (size_t i = 0; i < 5; ++i) {
                    if (event->watch.ticket != tickets[i].ticket
                            || event->watch.reactor != tickets[i].reactor)
                        continue;
                    check(!seen[i]);
                    seen[i] = 1;
                    matched = 1;
                    break;
                }
                check(matched);
            }
            consumed += got;
            if (!got) nanosleep(&pause, NULL); // trusted host test only
        }
        check(consumed == 5);
        check(ores_native_vm_nio_try_pop_batch(batch_vm, completions, 5) == 0);
        for (size_t i = 0; i < 5; ++i) {
            close(pipes[i][0]);
            close(pipes[i][1]);
        }
        check(ores_native_vm_close(batch_vm));
    }

    // Bare VM NIO is outer-host plumbing. Even with an already attached
    // backend, an actor carrier cannot bypass the ActorRef resource/continuation
    // ownership path. The actual VM ActorRef resource loan also fences DONE.
    {
        ActorNioProbe probe = {
            .vm = NULL,
            .fd = -1,
            .attempted = 0,
            .bare_watch_result = -1
        };
        OresNativeVMConfig actor_cfg = config();
        actor_cfg.domains[ORES_NATIVE_VM_MAIN].on_message = on_actor_nio_probe;
        actor_cfg.domains[ORES_NATIVE_VM_MAIN].context = &probe;

        OresNativeVM* actor_vm = ores_native_vm_open(&actor_cfg);
        check(actor_vm != NULL);
        check(ores_native_vm_enable_nio(actor_vm, 1, 0, 1));
        int actor_pipe[2] = {-1, -1};
        check(pipe2(actor_pipe, O_NONBLOCK | O_CLOEXEC) == 0);
        probe.vm = actor_vm;
        probe.fd = actor_pipe[0];

        const OresNativeActorRef actor_ref =
            ores_native_vm_spawn(actor_vm, ORES_NATIVE_VM_MAIN, 0);
        check(actor_ref.scheduler_domain != 0);
        check(ores_native_vm_try_send(
            actor_vm, ORES_NATIVE_VM_MAIN, actor_ref, UINT64_C(77))
            == ORES_NATIVE_SEND_ACCEPTED);
        for (int retry = 0; retry < 3000
                && !atomic_load_explicit(&probe.attempted, memory_order_acquire);
                ++retry) {
            struct timespec pause = {0, 1000000};
            nanosleep(&pause, NULL);
        }
        check(atomic_load_explicit(&probe.attempted, memory_order_acquire));
        check(atomic_load_explicit(
            &probe.bare_watch_result, memory_order_acquire) == 0);
        check(ores_native_vm_nio_started(actor_vm) == 0);

        // Legacy VM construction is intentionally wake-free: the new wake
        // table is opt-in and must not silently change existing VM semantics.
        OresNativeWakeTicket unsupported_wake = {0};
        check(!ores_native_vm_actor_reserve_wake(
            actor_vm, ORES_NATIVE_VM_MAIN, actor_ref, actor_ref,
            UINT64_C(1), UINT64_C(1), UINT64_C(1), UINT64_C(1),
            &unsupported_wake));
        check(unsupported_wake.slot_generation == 0);
        check(ores_native_vm_actor_outstanding_wake_loans(
            actor_vm, ORES_NATIVE_VM_MAIN, actor_ref) == 0);

        check(ores_native_vm_actor_resource_retain(
            actor_vm, ORES_NATIVE_VM_MAIN, actor_ref));
        check(ores_native_vm_stop_sealed(
            actor_vm, ORES_NATIVE_VM_MAIN, actor_ref));
        check(!ores_native_vm_actor_resource_retain(
            actor_vm, ORES_NATIVE_VM_MAIN, actor_ref));
        check(!ores_native_vm_poll_done(
            actor_vm, ORES_NATIVE_VM_MAIN, actor_ref));

        OresNativeActorRef stale = actor_ref;
        ++stale.generation;
        check(!ores_native_vm_actor_resource_release(
            actor_vm, ORES_NATIVE_VM_MAIN, stale));
        check(!ores_native_vm_poll_done(
            actor_vm, ORES_NATIVE_VM_MAIN, actor_ref));

        check(ores_native_vm_actor_resource_release(
            actor_vm, ORES_NATIVE_VM_MAIN, actor_ref));
        check(!ores_native_vm_actor_resource_release(
            actor_vm, ORES_NATIVE_VM_MAIN, actor_ref));
        check(ores_native_vm_wait_done(
            actor_vm, ORES_NATIVE_VM_MAIN, actor_ref));
        check(ores_native_vm_poll_done(
            actor_vm, ORES_NATIVE_VM_MAIN, actor_ref));

        close(actor_pipe[0]);
        close(actor_pipe[1]);
        check(ores_native_vm_close(actor_vm));
    }


    // Actor-await readiness handoff: reserve wake escrow before epoll, then
    // publish the terminal completion onto the producer's single-flight lane.
    // Plain completion ACK is forbidden because it would bypass the wake loan.
    {
        OwnedActorNioAwaitProbe probe = {
            .vm = NULL,
            .domain = ORES_NATIVE_VM_MAIN,
            .actor = {0},
            .fd = -1,
            .active = 0,
            .entered = 0,
            .go = 0,
            .attempted = 0,
            .accepted = 0,
            .duplicate_rejected = 0,
            .wakes = 0,
            .wake_outcome = 0,
            .wake_errors = 0,
            .rearm_attempted = 0,
            .rearm_accepted = 0,
            .rearm_watch = {0},
            .await = {0}
        };
        OresNativeVMConfig actor_cfg = config();
        actor_cfg.domains[ORES_NATIVE_VM_MAIN].on_message =
            on_owned_actor_nio_await;
        actor_cfg.domains[ORES_NATIVE_VM_MAIN].context = &probe;
        OresNativeVMWakeConfig wake_cfg = {0};
        wake_cfg.domains[ORES_NATIVE_VM_MAIN] = on_owned_actor_nio_wake;

        OresNativeVM* actor_vm =
            ores_native_vm_open_with_wakes(&actor_cfg, &wake_cfg);
        check(actor_vm != NULL);
        check(ores_native_vm_enable_nio(actor_vm, 1, 0, 1));
        int actor_pipe[2] = {-1, -1};
        check(pipe2(actor_pipe, O_NONBLOCK | O_CLOEXEC) == 0);
        probe.vm = actor_vm;
        probe.fd = actor_pipe[0];
        probe.actor = ores_native_vm_spawn(
            actor_vm, ORES_NATIVE_VM_MAIN, 0);
        check(probe.actor.scheduler_domain != 0);

        // VM wake authority is same-scheduler only in this slice. Do not
        // surface the scheduler's internal cross-scheduler bridge mode.
        OresNativeActorRef foreign_target = probe.actor;
        ++foreign_target.scheduler_domain;
        OresNativeWakeTicket cross_target = {0};
        check(!ores_native_vm_actor_reserve_wake(
            actor_vm, ORES_NATIVE_VM_MAIN, probe.actor, foreign_target,
            UINT64_C(201), UINT64_C(1), UINT64_C(1), UINT64_C(1),
            &cross_target));
        check(cross_target.slot_generation == 0);

        check(ores_native_vm_try_send(
            actor_vm, ORES_NATIVE_VM_MAIN, probe.actor, UINT64_C(99))
            == ORES_NATIVE_SEND_ACCEPTED);
        while (!atomic_load_explicit(&probe.entered, memory_order_acquire))
            sched_yield();
        atomic_store_explicit(&probe.go, 1, memory_order_release);
        while (!atomic_load_explicit(&probe.attempted, memory_order_acquire))
            sched_yield();
        check(atomic_load_explicit(&probe.accepted, memory_order_acquire));
        check(atomic_load_explicit(
            &probe.duplicate_rejected, memory_order_acquire));
        check(same_actor_ref(probe.await.watch.actor, probe.actor));
        check(same_actor_ref(probe.await.wake.producer, probe.actor));
        check(same_actor_ref(probe.await.wake.target, probe.actor));
        check(probe.await.wake.continuation_slot == UINT64_C(101));
        check(probe.await.wake.continuation_generation == UINT64_C(5));
        check(probe.await.wake.code_generation == UINT64_C(7));
        check(probe.await.wake.descriptor_generation == UINT64_C(11));
        check(ores_native_vm_actor_outstanding_wake_loans(
            actor_vm, ORES_NATIVE_VM_MAIN, probe.actor) == 1);

        check(write(actor_pipe[1], "r", 1) == 1);
        OresNativeActorNioCompletion ready =
            await_actor_completion(actor_vm);
        check(ready.io.outcome == ORES_NATIVE_NIO_READY);

        // Await-bound claims cannot skip scheduler handoff via plain ACK.
        check(!ores_native_vm_nio_ack_actor_completion(actor_vm, ready));
        check(atomic_load_explicit(&probe.wakes, memory_order_acquire) == 0);
        check(ores_native_vm_nio_dispatch_actor_await(actor_vm, ready)
            == ORES_NATIVE_NIO_AWAIT_PUBLISHED);
        check(ores_native_vm_nio_dispatch_actor_await(actor_vm, ready)
            == ORES_NATIVE_NIO_AWAIT_INVALID);

        struct timespec pause = {0, 1000000};
        for (int i = 0; i < 3000
                && atomic_load_explicit(&probe.wakes, memory_order_acquire) < 1;
                ++i)
            nanosleep(&pause, NULL);
        check(atomic_load_explicit(&probe.wakes, memory_order_acquire) == 1);
        check(atomic_load_explicit(&probe.wake_errors, memory_order_acquire) == 0);
        check(atomic_load_explicit(&probe.wake_outcome, memory_order_acquire)
            == ORES_NATIVE_WAKE_READY);
        check(atomic_load_explicit(
            &probe.rearm_attempted, memory_order_acquire));
        check(atomic_load_explicit(
            &probe.rearm_accepted, memory_order_acquire));
        check(probe.rearm_watch.claim_slot != 0);
        check(ores_native_vm_actor_outstanding_wake_loans(
            actor_vm, ORES_NATIVE_VM_MAIN, probe.actor) == 0);

        // The original pipe byte is still readable, so the callback's rearm
        // should produce a second one-shot READY. Settle its ordinary actor
        // resource claim explicitly before STOP.
        OresNativeActorNioCompletion rearmed =
            await_actor_completion(actor_vm);
        check(rearmed.io.outcome == ORES_NATIVE_NIO_READY);
        check(rearmed.io.fd == actor_pipe[0]);
        check(rearmed.owner.claim_generation
            == probe.rearm_watch.claim_generation);
        check(ores_native_vm_nio_ack_actor_completion(
            actor_vm, rearmed));

        check(ores_native_vm_stop_sealed(
            actor_vm, ORES_NATIVE_VM_MAIN, probe.actor));
        check(ores_native_vm_wait_done(
            actor_vm, ORES_NATIVE_VM_MAIN, probe.actor));
        close(actor_pipe[0]);
        close(actor_pipe[1]);
        check(ores_native_vm_close(actor_vm));
    }

    // Cancellation uses the same scheduler handoff and resumes exactly once
    // with CANCELLED rather than running anything on the completion consumer.
    {
        OwnedActorNioAwaitProbe probe = {
            .vm = NULL,
            .domain = ORES_NATIVE_VM_MAIN,
            .actor = {0},
            .fd = -1,
            .active = 0,
            .entered = 0,
            .go = 0,
            .attempted = 0,
            .accepted = 0,
            .duplicate_rejected = 0,
            .wakes = 0,
            .wake_outcome = 0,
            .wake_errors = 0,
            .rearm_attempted = 0,
            .rearm_accepted = 0,
            .rearm_watch = {0},
            .await = {0}
        };
        OresNativeVMConfig actor_cfg = config();
        actor_cfg.domains[ORES_NATIVE_VM_MAIN].on_message =
            on_owned_actor_nio_await;
        actor_cfg.domains[ORES_NATIVE_VM_MAIN].context = &probe;
        OresNativeVMWakeConfig wake_cfg = {0};
        wake_cfg.domains[ORES_NATIVE_VM_MAIN] = on_owned_actor_nio_wake;
        OresNativeVM* actor_vm =
            ores_native_vm_open_with_wakes(&actor_cfg, &wake_cfg);
        check(actor_vm != NULL);
        check(ores_native_vm_enable_nio(actor_vm, 1, 0, 1));
        int actor_pipe[2] = {-1, -1};
        check(pipe2(actor_pipe, O_NONBLOCK | O_CLOEXEC) == 0);
        probe.vm = actor_vm;
        probe.fd = actor_pipe[0];
        probe.actor = ores_native_vm_spawn(
            actor_vm, ORES_NATIVE_VM_MAIN, 0);
        check(probe.actor.scheduler_domain != 0);
        check(ores_native_vm_try_send(
            actor_vm, ORES_NATIVE_VM_MAIN, probe.actor, UINT64_C(99))
            == ORES_NATIVE_SEND_ACCEPTED);
        while (!atomic_load_explicit(&probe.entered, memory_order_acquire))
            sched_yield();
        atomic_store_explicit(&probe.go, 1, memory_order_release);
        while (!atomic_load_explicit(&probe.attempted, memory_order_acquire))
            sched_yield();
        check(atomic_load_explicit(&probe.accepted, memory_order_acquire));

        check(ores_native_vm_nio_try_cancel_actor(
            actor_vm, probe.await.watch));
        OresNativeActorNioCompletion cancelled =
            await_actor_completion(actor_vm);
        check(cancelled.io.outcome == ORES_NATIVE_NIO_CANCELLED);
        check(ores_native_vm_nio_dispatch_actor_await(
            actor_vm, cancelled) == ORES_NATIVE_NIO_AWAIT_PUBLISHED);

        struct timespec pause = {0, 1000000};
        for (int i = 0; i < 3000
                && atomic_load_explicit(&probe.wakes, memory_order_acquire) < 1;
                ++i)
            nanosleep(&pause, NULL);
        check(atomic_load_explicit(&probe.wakes, memory_order_acquire) == 1);
        check(atomic_load_explicit(&probe.wake_outcome, memory_order_acquire)
            == ORES_NATIVE_WAKE_CANCELLED);
        check(!atomic_load_explicit(&probe.wake_errors, memory_order_acquire));

        check(ores_native_vm_stop_sealed(
            actor_vm, ORES_NATIVE_VM_MAIN, probe.actor));
        check(ores_native_vm_wait_done(
            actor_vm, ORES_NATIVE_VM_MAIN, probe.actor));
        close(actor_pipe[0]);
        close(actor_pipe[1]);
        check(ores_native_vm_close(actor_vm));
    }

    // STOP before the irreversible wake transfer wins without fabricating a
    // resume. The reserved wake and NIO resource claim are both consumed, and
    // physical DONE becomes observable only after that cleanup.
    {
        OwnedActorNioAwaitProbe probe = {
            .vm = NULL,
            .domain = ORES_NATIVE_VM_MAIN,
            .actor = {0},
            .fd = -1,
            .active = 0,
            .entered = 0,
            .go = 0,
            .attempted = 0,
            .accepted = 0,
            .duplicate_rejected = 0,
            .wakes = 0,
            .wake_outcome = 0,
            .wake_errors = 0,
            .rearm_attempted = 0,
            .rearm_accepted = 0,
            .rearm_watch = {0},
            .await = {0}
        };
        OresNativeVMConfig actor_cfg = config();
        actor_cfg.domains[ORES_NATIVE_VM_MAIN].on_message =
            on_owned_actor_nio_await;
        actor_cfg.domains[ORES_NATIVE_VM_MAIN].context = &probe;
        OresNativeVMWakeConfig wake_cfg = {0};
        wake_cfg.domains[ORES_NATIVE_VM_MAIN] = on_owned_actor_nio_wake;
        OresNativeVM* actor_vm =
            ores_native_vm_open_with_wakes(&actor_cfg, &wake_cfg);
        check(actor_vm != NULL);
        check(ores_native_vm_enable_nio(actor_vm, 1, 0, 1));
        int actor_pipe[2] = {-1, -1};
        check(pipe2(actor_pipe, O_NONBLOCK | O_CLOEXEC) == 0);
        probe.vm = actor_vm;
        probe.fd = actor_pipe[0];
        probe.actor = ores_native_vm_spawn(
            actor_vm, ORES_NATIVE_VM_MAIN, 0);
        check(probe.actor.scheduler_domain != 0);
        check(ores_native_vm_try_send(
            actor_vm, ORES_NATIVE_VM_MAIN, probe.actor, UINT64_C(99))
            == ORES_NATIVE_SEND_ACCEPTED);
        while (!atomic_load_explicit(&probe.entered, memory_order_acquire))
            sched_yield();
        atomic_store_explicit(&probe.go, 1, memory_order_release);
        while (!atomic_load_explicit(&probe.attempted, memory_order_acquire))
            sched_yield();
        check(atomic_load_explicit(&probe.accepted, memory_order_acquire));

        check(ores_native_vm_stop_sealed(
            actor_vm, ORES_NATIVE_VM_MAIN, probe.actor));
        check(!ores_native_vm_poll_done(
            actor_vm, ORES_NATIVE_VM_MAIN, probe.actor));
        check(ores_native_vm_nio_try_cancel_actor(
            actor_vm, probe.await.watch));
        OresNativeActorNioCompletion terminal =
            await_actor_completion(actor_vm);
        check(terminal.io.outcome == ORES_NATIVE_NIO_CANCELLED);
        check(ores_native_vm_nio_dispatch_actor_await(actor_vm, terminal)
            == ORES_NATIVE_NIO_AWAIT_PRODUCER_RETIRED);
        check(atomic_load_explicit(&probe.wakes, memory_order_acquire) == 0);
        check(ores_native_vm_actor_outstanding_wake_loans(
            actor_vm, ORES_NATIVE_VM_MAIN, probe.actor) == 0);
        check(ores_native_vm_wait_done(
            actor_vm, ORES_NATIVE_VM_MAIN, probe.actor));

        close(actor_pipe[0]);
        close(actor_pipe[1]);
        check(ores_native_vm_close(actor_vm));
    }

    // VM close is an abandonment boundary for an await that never completed.
    // It must cancel the still-reserved wake before scheduler drain; otherwise
    // close would hang forever on the wake loan.
    {
        OwnedActorNioAwaitProbe probe = {
            .vm = NULL,
            .domain = ORES_NATIVE_VM_MAIN,
            .actor = {0},
            .fd = -1,
            .active = 0,
            .entered = 0,
            .go = 0,
            .attempted = 0,
            .accepted = 0,
            .duplicate_rejected = 0,
            .wakes = 0,
            .wake_outcome = 0,
            .wake_errors = 0,
            .rearm_attempted = 0,
            .rearm_accepted = 0,
            .rearm_watch = {0},
            .await = {0}
        };
        OresNativeVMConfig actor_cfg = config();
        actor_cfg.domains[ORES_NATIVE_VM_MAIN].on_message =
            on_owned_actor_nio_await;
        actor_cfg.domains[ORES_NATIVE_VM_MAIN].context = &probe;
        OresNativeVMWakeConfig wake_cfg = {0};
        wake_cfg.domains[ORES_NATIVE_VM_MAIN] = on_owned_actor_nio_wake;
        OresNativeVM* actor_vm =
            ores_native_vm_open_with_wakes(&actor_cfg, &wake_cfg);
        check(actor_vm != NULL);
        check(ores_native_vm_enable_nio(actor_vm, 1, 0, 1));
        int actor_pipe[2] = {-1, -1};
        check(pipe2(actor_pipe, O_NONBLOCK | O_CLOEXEC) == 0);
        probe.vm = actor_vm;
        probe.fd = actor_pipe[0];
        probe.actor = ores_native_vm_spawn(
            actor_vm, ORES_NATIVE_VM_MAIN, 0);
        check(probe.actor.scheduler_domain != 0);
        check(ores_native_vm_try_send(
            actor_vm, ORES_NATIVE_VM_MAIN, probe.actor, UINT64_C(99))
            == ORES_NATIVE_SEND_ACCEPTED);
        while (!atomic_load_explicit(&probe.entered, memory_order_acquire))
            sched_yield();
        atomic_store_explicit(&probe.go, 1, memory_order_release);
        while (!atomic_load_explicit(&probe.attempted, memory_order_acquire))
            sched_yield();
        check(atomic_load_explicit(&probe.accepted, memory_order_acquire));
        check(ores_native_vm_actor_outstanding_wake_loans(
            actor_vm, ORES_NATIVE_VM_MAIN, probe.actor) == 1);

        check(ores_native_vm_close(actor_vm));
        check(atomic_load_explicit(&probe.wakes, memory_order_acquire) == 0);
        close(actor_pipe[0]);
        close(actor_pipe[1]);
    }


    // Exact-ActorRef VM NIO: the actor carrier may register only on behalf of
    // itself. READY keeps the resource loan and FD claim live until trusted
    // scheduler handoff ACK; stale tokens and bare host cancellation fail.
    {
        OwnedActorNioProbe probe = {
            .vm = NULL,
            .domain = ORES_NATIVE_VM_MAIN,
            .actor = {0},
            .fd = -1,
            .entered = 0,
            .go = 0,
            .attempted = 0,
            .accepted = 0,
            .watch = {0}
        };
        OresNativeVMConfig actor_cfg = config();
        actor_cfg.domains[ORES_NATIVE_VM_MAIN].on_message = on_owned_actor_nio;
        actor_cfg.domains[ORES_NATIVE_VM_MAIN].context = &probe;

        OresNativeVM* actor_vm = ores_native_vm_open(&actor_cfg);
        check(actor_vm != NULL);
        check(ores_native_vm_enable_nio(actor_vm, 1, 0, 1));
        int actor_pipe[2] = {-1, -1};
        check(pipe2(actor_pipe, O_NONBLOCK | O_CLOEXEC) == 0);
        probe.vm = actor_vm;
        probe.fd = actor_pipe[0];
        probe.actor = ores_native_vm_spawn(
            actor_vm, ORES_NATIVE_VM_MAIN, 0);
        check(probe.actor.scheduler_domain != 0);

        check(ores_native_vm_try_send(
            actor_vm, ORES_NATIVE_VM_MAIN, probe.actor, UINT64_C(88))
            == ORES_NATIVE_SEND_ACCEPTED);
        while (!atomic_load_explicit(&probe.entered, memory_order_acquire))
            sched_yield();
        atomic_store_explicit(&probe.go, 1, memory_order_release);
        while (!atomic_load_explicit(&probe.attempted, memory_order_acquire))
            sched_yield();
        check(atomic_load_explicit(&probe.accepted, memory_order_acquire));
        check(same_actor_ref(probe.watch.actor, probe.actor));
        check(probe.watch.domain == ORES_NATIVE_VM_MAIN);
        check(probe.watch.claim_slot != 0 && probe.watch.claim_generation != 0);

        // Bare host cancellation is not authority over actor-owned I/O.
        check(!ores_native_vm_nio_try_cancel(actor_vm, probe.watch.watch));

        OresNativeActorNioWatch stale_watch = probe.watch;
        ++stale_watch.actor.generation;
        check(!ores_native_vm_nio_try_cancel_actor(actor_vm, stale_watch));
        stale_watch = probe.watch;
        ++stale_watch.claim_generation;
        check(!ores_native_vm_nio_try_cancel_actor(actor_vm, stale_watch));

        check(write(actor_pipe[1], "a", 1) == 1);
        OresNativeActorNioCompletion ready =
            await_actor_completion(actor_vm);
        check(ready.io.outcome == ORES_NATIVE_NIO_READY);
        check(ready.io.fd == actor_pipe[0]);
        check(same_actor_ref(ready.owner.actor, probe.actor));
        check(ready.owner.claim_generation == probe.watch.claim_generation);

        check(ores_native_vm_stop_sealed(
            actor_vm, ORES_NATIVE_VM_MAIN, probe.actor));
        check(!ores_native_vm_poll_done(
            actor_vm, ORES_NATIVE_VM_MAIN, probe.actor));

        // epoll already retired the one-shot watch, but the FD stays reserved
        // until continuation/scheduler handoff ACK releases the actor loan.
        OresNativeNioWatch forbidden_reuse = {0};
        check(!ores_native_vm_nio_try_watch(
            actor_vm, actor_pipe[0], EPOLLIN, &forbidden_reuse));

        OresNativeActorNioCompletion stale_ready = ready;
        ++stale_ready.owner.actor.generation;
        check(!ores_native_vm_nio_ack_actor_completion(
            actor_vm, stale_ready));
        stale_ready = ready;
        ++stale_ready.owner.claim_generation;
        check(!ores_native_vm_nio_ack_actor_completion(
            actor_vm, stale_ready));

        check(ores_native_vm_nio_ack_actor_completion(actor_vm, ready));
        check(!ores_native_vm_nio_ack_actor_completion(actor_vm, ready));
        check(ores_native_vm_wait_done(
            actor_vm, ORES_NATIVE_VM_MAIN, probe.actor));

        // ACK releases the wrapper-level FD ownership fence.
        OresNativeNioWatch reused = {0};
        check(ores_native_vm_nio_try_watch(
            actor_vm, actor_pipe[0], EPOLLIN, &reused));
        check(ores_native_vm_nio_try_cancel(actor_vm, reused));
        OresNativeNioCompletion host_cancel = await_completion(actor_vm);
        check(host_cancel.watch.ticket == reused.ticket);
        check(host_cancel.outcome == ORES_NATIVE_NIO_CANCELLED);

        close(actor_pipe[0]);
        close(actor_pipe[1]);
        check(ores_native_vm_close(actor_vm));
    }

    // Actor-owned cancellation still holds physical DONE until the CANCELLED
    // terminal record is transferred/ACKed. Wrong actor/claim capabilities
    // cannot cancel or acknowledge it.
    {
        OwnedActorNioProbe probe = {
            .vm = NULL,
            .domain = ORES_NATIVE_VM_MAIN,
            .actor = {0},
            .fd = -1,
            .entered = 0,
            .go = 0,
            .attempted = 0,
            .accepted = 0,
            .watch = {0}
        };
        OresNativeVMConfig actor_cfg = config();
        actor_cfg.domains[ORES_NATIVE_VM_MAIN].on_message = on_owned_actor_nio;
        actor_cfg.domains[ORES_NATIVE_VM_MAIN].context = &probe;
        OresNativeVM* actor_vm = ores_native_vm_open(&actor_cfg);
        check(actor_vm != NULL);
        check(ores_native_vm_enable_nio(actor_vm, 1, 0, 1));
        int actor_pipe[2] = {-1, -1};
        check(pipe2(actor_pipe, O_NONBLOCK | O_CLOEXEC) == 0);
        probe.vm = actor_vm;
        probe.fd = actor_pipe[0];
        probe.actor = ores_native_vm_spawn(
            actor_vm, ORES_NATIVE_VM_MAIN, 0);
        check(probe.actor.scheduler_domain != 0);
        check(ores_native_vm_try_send(
            actor_vm, ORES_NATIVE_VM_MAIN, probe.actor, UINT64_C(88))
            == ORES_NATIVE_SEND_ACCEPTED);
        while (!atomic_load_explicit(&probe.entered, memory_order_acquire))
            sched_yield();
        atomic_store_explicit(&probe.go, 1, memory_order_release);
        while (!atomic_load_explicit(&probe.attempted, memory_order_acquire))
            sched_yield();
        check(atomic_load_explicit(&probe.accepted, memory_order_acquire));

        check(ores_native_vm_nio_try_cancel_actor(actor_vm, probe.watch));
        check(!ores_native_vm_nio_try_cancel_actor(actor_vm, probe.watch));
        OresNativeActorNioCompletion cancelled =
            await_actor_completion(actor_vm);
        check(cancelled.io.outcome == ORES_NATIVE_NIO_CANCELLED);

        check(ores_native_vm_stop_sealed(
            actor_vm, ORES_NATIVE_VM_MAIN, probe.actor));
        check(!ores_native_vm_poll_done(
            actor_vm, ORES_NATIVE_VM_MAIN, probe.actor));
        check(ores_native_vm_nio_ack_actor_completion(
            actor_vm, cancelled));
        check(ores_native_vm_wait_done(
            actor_vm, ORES_NATIVE_VM_MAIN, probe.actor));

        close(actor_pipe[0]);
        close(actor_pipe[1]);
        check(ores_native_vm_close(actor_vm));
    }

    // VM close is also a physical abandonment boundary. A watch that has not
    // produced/ACKed a completion must be stopped/joined and its exact actor
    // resource loan released before scheduler drain, otherwise close deadlocks.
    {
        OwnedActorNioProbe probe = {
            .vm = NULL,
            .domain = ORES_NATIVE_VM_MAIN,
            .actor = {0},
            .fd = -1,
            .entered = 0,
            .go = 0,
            .attempted = 0,
            .accepted = 0,
            .watch = {0}
        };
        OresNativeVMConfig actor_cfg = config();
        actor_cfg.domains[ORES_NATIVE_VM_MAIN].on_message = on_owned_actor_nio;
        actor_cfg.domains[ORES_NATIVE_VM_MAIN].context = &probe;
        OresNativeVM* actor_vm = ores_native_vm_open(&actor_cfg);
        check(actor_vm != NULL);
        check(ores_native_vm_enable_nio(actor_vm, 1, 0, 1));
        int actor_pipe[2] = {-1, -1};
        check(pipe2(actor_pipe, O_NONBLOCK | O_CLOEXEC) == 0);
        probe.vm = actor_vm;
        probe.fd = actor_pipe[0];
        probe.actor = ores_native_vm_spawn(
            actor_vm, ORES_NATIVE_VM_MAIN, 0);
        check(probe.actor.scheduler_domain != 0);
        check(ores_native_vm_try_send(
            actor_vm, ORES_NATIVE_VM_MAIN, probe.actor, UINT64_C(88))
            == ORES_NATIVE_SEND_ACCEPTED);
        while (!atomic_load_explicit(&probe.entered, memory_order_acquire))
            sched_yield();
        atomic_store_explicit(&probe.go, 1, memory_order_release);
        while (!atomic_load_explicit(&probe.attempted, memory_order_acquire))
            sched_yield();
        check(atomic_load_explicit(&probe.accepted, memory_order_acquire));

        check(ores_native_vm_close(actor_vm));
        close(actor_pipe[0]);
        close(actor_pipe[1]);
    }

    // STOP and actor NIO admission arbitrate on the scheduler resource loan.
    // Run a bounded adversarial loop through the actual actor-carrier bridge.
    {
        unsigned watch_wins = 0;
        unsigned stop_wins = 0;
        for (unsigned iteration = 0; iteration < 32; ++iteration) {
            OwnedActorNioProbe probe = {
                .vm = NULL,
                .domain = ORES_NATIVE_VM_MAIN,
                .actor = {0},
                .fd = -1,
                .entered = 0,
                .go = 0,
                .attempted = 0,
                .accepted = 0,
                .watch = {0}
            };
            OresNativeVMConfig actor_cfg = config();
            actor_cfg.domains[ORES_NATIVE_VM_MAIN].on_message =
                on_owned_actor_nio;
            actor_cfg.domains[ORES_NATIVE_VM_MAIN].context = &probe;
            OresNativeVM* actor_vm = ores_native_vm_open(&actor_cfg);
            check(actor_vm != NULL);
            check(ores_native_vm_enable_nio(actor_vm, 1, 0, 1));
            int actor_pipe[2] = {-1, -1};
            check(pipe2(actor_pipe, O_NONBLOCK | O_CLOEXEC) == 0);
            probe.vm = actor_vm;
            probe.fd = actor_pipe[0];
            probe.actor = ores_native_vm_spawn(
                actor_vm, ORES_NATIVE_VM_MAIN, 0);
            check(probe.actor.scheduler_domain != 0);
            check(ores_native_vm_try_send(
                actor_vm, ORES_NATIVE_VM_MAIN, probe.actor, UINT64_C(88))
                == ORES_NATIVE_SEND_ACCEPTED);
            while (!atomic_load_explicit(&probe.entered, memory_order_acquire))
                sched_yield();

            atomic_store_explicit(&probe.go, 1, memory_order_release);
            const int stopped = ores_native_vm_stop_sealed(
                actor_vm, ORES_NATIVE_VM_MAIN, probe.actor);
            check(stopped);
            while (!atomic_load_explicit(&probe.attempted, memory_order_acquire))
                sched_yield();

            if (atomic_load_explicit(&probe.accepted, memory_order_acquire)) {
                ++watch_wins;
                check(!ores_native_vm_poll_done(
                    actor_vm, ORES_NATIVE_VM_MAIN, probe.actor));
                check(ores_native_vm_nio_try_cancel_actor(
                    actor_vm, probe.watch));
                OresNativeActorNioCompletion completion =
                    await_actor_completion(actor_vm);
                check(completion.io.outcome == ORES_NATIVE_NIO_CANCELLED);
                check(ores_native_vm_nio_ack_actor_completion(
                    actor_vm, completion));
            } else {
                ++stop_wins;
            }
            check(ores_native_vm_wait_done(
                actor_vm, ORES_NATIVE_VM_MAIN, probe.actor));
            check(ores_native_vm_close(actor_vm));
            close(actor_pipe[0]);
            close(actor_pipe[1]);
        }
        check(watch_wins + stop_wins == 32);
    }


    // The VM bridge is bounded at the reactor's global 4x64 capacity. A 257th
    // actor watch must fail and refund its speculative resource retain. After
    // STOP, draining/ACKing the accepted 256 must be sufficient for DONE;
    // any retain leak would make wait_done hang/fail this test.
    {
        enum { actor_claims = 256 };
        OresNativeVM* actor_vm = ores_native_vm_open(&cfg);
        check(actor_vm != NULL);
        check(ores_native_vm_enable_nio(actor_vm, 4, 0, 1));
        const OresNativeActorRef actor_ref =
            ores_native_vm_spawn(actor_vm, ORES_NATIVE_VM_MAIN, 0);
        check(actor_ref.scheduler_domain != 0);

        int fds[actor_claims + 1];
        OresNativeActorNioWatch watches[actor_claims];
        for (size_t i = 0; i < actor_claims + 1; ++i) {
            fds[i] = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
            check(fds[i] >= 0);
        }
        for (size_t i = 0; i < actor_claims; ++i) {
            int admitted = 0;
            for (int retry = 0; retry < 3000 && !admitted; ++retry) {
                admitted = ores_native_vm_nio_try_watch_actor(
                    actor_vm, ORES_NATIVE_VM_MAIN, actor_ref,
                    fds[i], EPOLLIN, &watches[i]);
                if (!admitted) sched_yield();
            }
            check(admitted);
        }
        OresNativeActorNioWatch overflow = {0};
        check(!ores_native_vm_nio_try_watch_actor(
            actor_vm, ORES_NATIVE_VM_MAIN, actor_ref,
            fds[actor_claims], EPOLLIN, &overflow));

        check(ores_native_vm_stop_sealed(
            actor_vm, ORES_NATIVE_VM_MAIN, actor_ref));
        check(!ores_native_vm_poll_done(
            actor_vm, ORES_NATIVE_VM_MAIN, actor_ref));

        for (size_t i = 0; i < actor_claims; ++i)
            check(ores_native_vm_nio_try_cancel_actor(actor_vm, watches[i]));
        for (size_t i = 0; i < actor_claims; ++i) {
            OresNativeActorNioCompletion completion =
                await_actor_completion(actor_vm);
            check(completion.io.outcome == ORES_NATIVE_NIO_CANCELLED);
            check(same_actor_ref(completion.owner.actor, actor_ref));
            check(ores_native_vm_nio_ack_actor_completion(
                actor_vm, completion));
        }
        check(ores_native_vm_wait_done(
            actor_vm, ORES_NATIVE_VM_MAIN, actor_ref));
        for (size_t i = 0; i < actor_claims + 1; ++i) close(fds[i]);
        check(ores_native_vm_close(actor_vm));
    }

    // Once the first actor is admitted, adding a reactor is no longer a
    // legal VM bootstrap operation. No late selector thread may be created.
    OresNativeVM* live = ores_native_vm_open(&cfg);
    check(live != NULL);
    const OresNativeActorRef actor =
        ores_native_vm_spawn(live, ORES_NATIVE_VM_MAIN, 0);
    check(actor.scheduler_domain != 0);
    check(!ores_native_vm_nio_can_attach(live));
    check(!ores_native_vm_enable_nio(live, 4, 0, 1));
    check(ores_native_vm_close(live));

    // Explicit cold-start reservation must prevent actor admission during
    // selector creation, including a competing initializer.
    OresNativeVM* cold = ores_native_vm_open(&cfg);
    check(cold != NULL);
    check(ores_native_vm_nio_begin_bootstrap(cold));
    check(!ores_native_vm_nio_begin_bootstrap(cold));
    const OresNativeActorRef denied =
        ores_native_vm_spawn(cold, ORES_NATIVE_VM_MAIN, 0);
    check(denied.scheduler_domain == 0);
    pthread_t foreign;
    check(pthread_create(&foreign, NULL, foreign_abort_bootstrap, cold) == 0);
    check(pthread_join(foreign, NULL) == 0);
    check(!ores_native_vm_nio_can_attach(cold));
    // Only the trusted bootstrap owner can abort, then a fresh owner may
    // reserve again. This prevents cancellation laundering across carriers.
    ores_native_vm_nio_cancel_bootstrap(cold);
    check(ores_native_vm_nio_can_attach(cold));
    check(ores_native_vm_enable_nio(cold, 2, 0, 1));
    check(ores_native_vm_nio_started(cold) == 0);
    check(ores_native_vm_close(cold));

    // Fully preallocated RT bootstrap does not spawn network workers on its
    // deadline-governed operation path. This is not a WCET certification.
    vm = ores_native_vm_open_realtime(&cfg);
    check(vm != NULL);
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i)
        check(ores_native_vm_started_carriers(vm, (enum OresNativeVMDomain)i) == 2);
    check(ores_native_vm_enable_nio(vm, 3, 1, 3));
    check(ores_native_vm_nio_started(vm) == 3);
    size_t expected_rt_threads = 1 + ores_native_vm_nio_started(vm);
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i)
        expected_rt_threads += ores_native_vm_started_carriers(
            vm, (enum OresNativeVMDomain)i);
    const ThreadIdSet rt_threads = await_thread_count(expected_rt_threads);

    int rt_pipe[2] = {-1, -1};
    check(pipe2(rt_pipe, O_NONBLOCK | O_CLOEXEC) == 0);
    OresNativeNioWatch rt_watch = {0};
    check(ores_native_vm_nio_try_watch(vm, rt_pipe[0], EPOLLIN, &rt_watch));
    check(write(rt_pipe[1], "r", 1) == 1);
    const OresNativeNioCompletion rt_done = await_completion(vm);
    check(rt_done.watch.ticket == rt_watch.ticket);
    check(rt_done.watch.reactor == rt_watch.reactor);
    check(rt_done.outcome == ORES_NATIVE_NIO_READY);
    ores_native_vm_nio_drain_signal(vm, rt_done.watch.reactor);
    check_same_thread_ids(rt_threads, snapshot_thread_ids());
    close(rt_pipe[0]);
    close(rt_pipe[1]);
    check(ores_native_vm_close(vm));

    // Concurrent host shutdown must also wait for an in-progress reactor
    // bootstrap, even if no backend has been installed yet. A late selector
    // thread must never materialize inside a VM already freed by close.
    for (int iteration = 0; iteration < 25; ++iteration) {
        OresNativeVM* creating = ores_native_vm_open(&cfg);
        check(creating != NULL);
        check(ores_native_vm_nio_begin_bootstrap(creating));
        NioCloseRace race = {.vm = creating};
        atomic_init(&race.entered, 0);
        atomic_init(&race.finished, 0);
        pthread_t closer;
        check(pthread_create(&closer, NULL, close_while_leased, &race) == 0);
        while (!atomic_load(&race.entered)) {
            struct timespec delay = {0, 1000000};
            nanosleep(&delay, NULL);
        }
        await_retirement(creating);
        check(!atomic_load(&race.finished));
        // The owner cancels the bootstrap; external close may now retire it.
        ores_native_vm_nio_cancel_bootstrap(creating);
        check(pthread_join(closer, NULL) == 0);
        check(atomic_load(&race.finished));
    }

    // A reactor cannot be destroyed while a concurrent accepted VM host
    // operation still has a borrowed backend. This is a test-only host
    // pthread: actor, CONTROL and reactor worker threads never wait here.
    for (int iteration = 0; iteration < 25; ++iteration) {
        OresNativeVM* racing = ores_native_vm_open(&cfg);
        check(racing != NULL);
        check(ores_native_vm_enable_nio(racing, 2, 0, 1));
        void* borrowed = ores_native_vm_nio_acquire(racing);
        check(borrowed != NULL);
        NioCloseRace race = {.vm = racing};
        atomic_init(&race.entered, 0);
        atomic_init(&race.finished, 0);
        pthread_t closer;
        check(pthread_create(&closer, NULL, close_while_leased, &race) == 0);
        while (!atomic_load(&race.entered)) {
            struct timespec pause = {0, 1000000};
            nanosleep(&pause, NULL);
        }
        await_retirement(racing);
        check(!atomic_load(&race.finished));
        // VM retirement fences all new NIO acquisition but cannot free this
        // borrowed backend until the accepted operation releases its pin.
        check(ores_native_vm_nio_acquire(racing) == NULL);
        ores_native_vm_nio_release(racing, borrowed);
        check(pthread_join(closer, NULL) == 0);
        check(atomic_load(&race.finished));
    }
    return 0;
}
