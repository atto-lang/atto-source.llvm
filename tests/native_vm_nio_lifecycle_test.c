#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "oreslang/runtime/NativeOresVMNio.h"
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/epoll.h>
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
    // The reservation owner must not synchronously wait for itself. Rejection
    // is non-destructive: after cancelling, this same VM remains attachable.
    check(!ores_native_vm_close(cold));
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
