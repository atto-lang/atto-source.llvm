/* Registered NativeOresVM supervisor lifecycle + actor-owned frames.
 * HOST test, not source-level .atto supervisor/codegen certification.
 * The parent/child links, finalization receipts, and terminal CONTROL
 * callbacks must all pass through the real VM, not the bare scheduler. */
#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeOresVM.h"
#include "oreslang/runtime/NativeActorLogicalFramesV1.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define CHECK(c) do { if (!(c)) { \
  fprintf(stderr, "VM_SUPERVISED_QUOTA_FAIL line=%d expr=%s\n", __LINE__, #c); \
  abort(); } } while (0)
enum { SUPERVISOR = 0, ACTOR_A = 1, ACTOR_B = 2, ACTOR_A2 = 3 };
#define STACK_QUOTA_FAILURE UINT64_C(0xA7700001)
typedef struct {
    OresNativeActorLogicalFramesV1* a;
    OresNativeActorLogicalFramesV1* b;
    OresNativeActorLogicalFramesV1* a2;
    OresNativeActorRef parent, first, sibling, replacement;
    OresLogicalFrameV1 stale;
    atomic_int failed_a, b_42, new_a_ok, notices;
} State;

static void check_turn(void* raw, size_t actor, uint64_t payload) {
    State* s = (State*)raw;
    OresLogicalTurnV1 t = {0};
    OresLogicalFrameV1 frame = {0}, overflow = {0};
    if (actor == ACTOR_A) {
        CHECK(payload == 7);
        CHECK(ores_logical_frames_v1_begin(s->a, 701, 1, &t));
        CHECK(ores_logical_frames_v1_push(s->a, t, &frame));
        CHECK(!ores_logical_frames_v1_push(s->a, t, &overflow));
        s->stale = frame;
        CHECK(ores_logical_frames_v1_cancel(s->a, t));
        atomic_store_explicit(&s->failed_a, 1, memory_order_release);
    } else if (actor == ACTOR_B) {
        CHECK(payload == 42);
        CHECK(ores_logical_frames_v1_begin(s->b, 702, 1, &t));
        CHECK(ores_logical_frames_v1_push(s->b, t, &frame));
        int64_t v = 42, observed = 0;
        CHECK(ores_logical_frames_v1_write(s->b, frame, 0, &v, sizeof v));
        CHECK(ores_logical_frames_v1_read(s->b, frame, 0, &observed, sizeof observed));
        CHECK(observed == 42);
        CHECK(ores_logical_frames_v1_pop(s->b, frame));
        CHECK(ores_logical_frames_v1_end(s->b, t));
        atomic_store_explicit(&s->b_42, 1, memory_order_release);
    } else if (actor == ACTOR_A2) {
        CHECK(payload == 9);
        CHECK(ores_logical_frames_v1_begin(s->a2, 701, 2, &t));
        CHECK(!ores_logical_frames_v1_push(s->a2, s->stale.turn, &frame));
        CHECK(ores_logical_frames_v1_push(s->a2, t, &frame));
        int64_t observed = -1;
        CHECK(!ores_logical_frames_v1_read(s->a2, s->stale, 0, &observed, sizeof observed));
        CHECK(ores_logical_frames_v1_read(s->a2, frame, 0, &observed, sizeof observed));
        CHECK(observed == 0);
        CHECK(ores_logical_frames_v1_pop(s->a2, frame));
        CHECK(ores_logical_frames_v1_end(s->a2, t));
        atomic_store_explicit(&s->new_a_ok, 1, memory_order_release);
    } else {
        CHECK(0); /* supervisor accepts typed CONTROL only */
    }
}

static void on_supervisor_failure(void* raw, size_t parent,
                                  OresNativeActorTerminalFailure f) {
    State* s = (State*)raw;
    CHECK(parent == SUPERVISOR);
    int observed = atomic_fetch_add_explicit(&s->notices, 1, memory_order_acq_rel);
    if (observed == 0) {
        CHECK(f.kind == ORES_NATIVE_CONTROL_PANIC);
        CHECK(f.child.scheduler_domain == s->first.scheduler_domain);
        CHECK(f.child.actor_slot == s->first.actor_slot);
        CHECK(f.child.generation == s->first.generation);
        CHECK(f.failure_code == STACK_QUOTA_FAILURE);
        CHECK(f.correlation == 701);
        CHECK(atomic_load_explicit(&s->failed_a, memory_order_acquire) == 1);
    } else if (observed == 1) {
        CHECK(f.kind == ORES_NATIVE_CONTROL_STOPPED);
        CHECK(f.child.scheduler_domain == s->replacement.scheduler_domain);
        CHECK(f.child.actor_slot == s->replacement.actor_slot);
        CHECK(f.child.generation == s->replacement.generation);
        CHECK(f.failure_code == 0);
        CHECK(f.correlation == 702);
    } else {
        CHECK(0); /* no duplicate terminal notice */
    }
}
static void await_one(const atomic_int* flag) {
    for (int i = 0; i < 5000; ++i) {
        if (atomic_load_explicit(flag, memory_order_acquire)) return;
        const struct timespec nap = {0, 1000000};
        nanosleep(&nap, NULL);
    }
    CHECK(0);
}
int main(void) {
    State s = {0};
    atomic_init(&s.failed_a, 0);
    atomic_init(&s.b_42, 0);
    atomic_init(&s.new_a_ok, 0);
    atomic_init(&s.notices, 0);
    s.a = ores_logical_frames_v1_open(701, 1, 1, 1, sizeof(int64_t));
    s.b = ores_logical_frames_v1_open(702, 1, 1, 1, sizeof(int64_t));
    s.a2 = ores_logical_frames_v1_open(701, 2, 1, 1, sizeof(int64_t));
    CHECK(s.a && s.b && s.a2);
    OresNativeVMConfig cfg = {0};
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i) {
        cfg.domains[i].actor_slots = i == ORES_NATIVE_VM_MAIN ? 4 : 1;
        cfg.domains[i].mailbox_capacity = 4;
        cfg.domains[i].carrier_workers = i == ORES_NATIVE_VM_MAIN ? 3 : 1;
        cfg.domains[i].on_message = check_turn;
        cfg.domains[i].on_failure = on_supervisor_failure;
        cfg.domains[i].context = &s;
    }
    OresNativeVM* vm = ores_native_vm_open(&cfg);
    CHECK(vm);
    s.parent = ores_native_vm_spawn(vm, ORES_NATIVE_VM_MAIN, SUPERVISOR);
    CHECK(s.parent.scheduler_domain);
    s.first = ores_native_vm_spawn_supervised(vm, ORES_NATIVE_VM_MAIN, ACTOR_A, s.parent);
    s.sibling = ores_native_vm_spawn(vm, ORES_NATIVE_VM_MAIN, ACTOR_B);
    CHECK(s.first.scheduler_domain && s.sibling.scheduler_domain);
    OresNativeActorRef p = ores_native_vm_supervisor_of(vm, ORES_NATIVE_VM_MAIN, s.first);
    CHECK(p.scheduler_domain == s.parent.scheduler_domain && p.generation == s.parent.generation);
    CHECK(p.actor_slot == s.parent.actor_slot);
    CHECK(!ores_native_vm_stop(vm, ORES_NATIVE_VM_MAIN, s.parent)); /* live child gate */

    OresNativeActorTerminalFailure first = {
        .kind = ORES_NATIVE_CONTROL_PANIC, .child = s.first,
        .failure_code = STACK_QUOTA_FAILURE, .correlation = 701
    };
    CHECK(!ores_native_vm_confirm_supervised_finalized(vm, ORES_NATIVE_VM_MAIN, s.first));
    CHECK(!ores_native_vm_publish_supervised_failure(vm, ORES_NATIVE_VM_MAIN, first));
    CHECK(ores_native_vm_try_send(vm, ORES_NATIVE_VM_MAIN, s.first, 7) == ORES_NATIVE_SEND_ACCEPTED);
    CHECK(ores_native_vm_try_send(vm, ORES_NATIVE_VM_MAIN, s.sibling, 42) == ORES_NATIVE_SEND_ACCEPTED);
    await_one(&s.failed_a);
    await_one(&s.b_42);
    CHECK(!ores_native_vm_publish_supervised_failure(vm, ORES_NATIVE_VM_MAIN, first));
    CHECK(ores_native_vm_stop_sealed(vm, ORES_NATIVE_VM_MAIN, s.first));
    CHECK(ores_native_vm_wait_done(vm, ORES_NATIVE_VM_MAIN, s.first));
    /* STOP alone cannot publish failure until actual host-owned cleanup. */
    CHECK(!ores_native_vm_publish_supervised_failure(vm, ORES_NATIVE_VM_MAIN, first));
    CHECK(ores_logical_frames_v1_close(s.a));
    s.a = NULL;
    CHECK(ores_native_vm_confirm_supervised_finalized(vm, ORES_NATIVE_VM_MAIN, s.first));
    CHECK(!ores_native_vm_confirm_supervised_finalized(vm, ORES_NATIVE_VM_MAIN, s.first));
    OresNativeActorTerminalFailure forged = first;
    ++forged.child.generation;
    CHECK(!ores_native_vm_publish_supervised_failure(vm, ORES_NATIVE_VM_MAIN, forged));
    CHECK(ores_native_vm_publish_supervised_failure(vm, ORES_NATIVE_VM_MAIN, first));
    CHECK(!ores_native_vm_publish_supervised_failure(vm, ORES_NATIVE_VM_MAIN, first));
    await_one(&s.notices);

    /* Restart is explicitly host-driven in this stage, but the replacement
     * is also registered under the same supervisor for terminal accounting. */
    s.replacement = ores_native_vm_spawn_supervised(vm, ORES_NATIVE_VM_MAIN, ACTOR_A2, s.parent);
    CHECK(s.replacement.scheduler_domain);
    CHECK(ores_native_vm_try_send(vm, ORES_NATIVE_VM_MAIN, s.first, 8) != ORES_NATIVE_SEND_ACCEPTED);
    CHECK(ores_native_vm_try_send(vm, ORES_NATIVE_VM_MAIN, s.replacement, 9) == ORES_NATIVE_SEND_ACCEPTED);
    await_one(&s.new_a_ok);
    CHECK(ores_native_vm_stop_sealed(vm, ORES_NATIVE_VM_MAIN, s.replacement));
    CHECK(ores_native_vm_wait_done(vm, ORES_NATIVE_VM_MAIN, s.replacement));
    CHECK(ores_logical_frames_v1_close(s.a2));
    CHECK(ores_native_vm_confirm_supervised_finalized(vm, ORES_NATIVE_VM_MAIN, s.replacement));
    OresNativeActorTerminalFailure second = {
        .kind = ORES_NATIVE_CONTROL_STOPPED, .child = s.replacement,
        .failure_code = 0, .correlation = 702
    };
    CHECK(ores_native_vm_publish_supervised_failure(vm, ORES_NATIVE_VM_MAIN, second));
    for (int i = 0; i < 5000 && atomic_load_explicit(&s.notices, memory_order_acquire) < 2; ++i) {
        const struct timespec nap = {0, 1000000};
        nanosleep(&nap, NULL);
    }
    CHECK(atomic_load_explicit(&s.notices, memory_order_acquire) == 2);
    CHECK(ores_native_vm_stop(vm, ORES_NATIVE_VM_MAIN, s.sibling));
    CHECK(ores_native_vm_wait_done(vm, ORES_NATIVE_VM_MAIN, s.sibling));
    CHECK(ores_logical_frames_v1_close(s.b));
    CHECK(ores_native_vm_stop(vm, ORES_NATIVE_VM_MAIN, s.parent));
    CHECK(ores_native_vm_wait_done(vm, ORES_NATIVE_VM_MAIN, s.parent));
    CHECK(ores_native_vm_close(vm));
    puts("NATIVE_VM_SUPERVISED_QUOTA_PASS finalized=1 notices=2 B=42 replacement=1");
    return 0;
}
