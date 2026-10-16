#define _GNU_SOURCE
#include "oreslang/runtime/NativeOresVMNio.h"
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <time.h>
#include <unistd.h>

static void check(int valid) { if (!valid) abort(); }
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

    // Fully preallocated RT bootstrap does not spawn network workers on its
    // deadline-governed operation path. This is not a WCET certification.
    vm = ores_native_vm_open_realtime(&cfg);
    check(vm != NULL);
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i)
        check(ores_native_vm_started_carriers(vm, (enum OresNativeVMDomain)i) == 2);
    check(ores_native_vm_enable_nio(vm, 3, 1, 3));
    check(ores_native_vm_nio_started(vm) == 3);
    check(ores_native_vm_close(vm));
    return 0;
}
