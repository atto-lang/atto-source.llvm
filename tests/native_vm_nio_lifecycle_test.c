#define _GNU_SOURCE
#include "oreslang/runtime/NativeOresVMNio.h"
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <time.h>
#include <unistd.h>

static void check(int valid) { if (!valid) abort(); }
typedef struct {
    OresNativeVM* vm;
    atomic_int entered;
    atomic_int finished;
} NioCloseRace;
static void* close_while_leased(void* opaque) {
    NioCloseRace* race = (NioCloseRace*)opaque;
    atomic_store(&race->entered, 1);
    check(ores_native_vm_close(race->vm));
    atomic_store(&race->finished, 1);
    return NULL;
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
    abort();
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
    check(ores_native_vm_nio_started(vm) == 1);
    int pipe_fd[2] = {-1, -1};
    check(pipe2(pipe_fd, O_NONBLOCK | O_CLOEXEC) == 0);
    OresNativeNioWatch watch = {0};
    check(ores_native_vm_nio_try_watch(vm, pipe_fd[0], EPOLLIN, &watch));
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

    // Fully preallocated RT bootstrap does not spawn network workers on its
    // deadline-governed operation path. This is not a WCET certification.
    vm = ores_native_vm_open_realtime(&cfg);
    check(vm != NULL);
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i)
        check(ores_native_vm_started_carriers(vm, (enum OresNativeVMDomain)i) == 2);
    check(ores_native_vm_enable_nio(vm, 3, 1, 3));
    check(ores_native_vm_nio_started(vm) == 3);
    check(ores_native_vm_close(vm));

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
        struct timespec pause = {0, 10000000};
        nanosleep(&pause, NULL);
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
