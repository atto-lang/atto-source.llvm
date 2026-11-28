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
#include <sched.h>
#include <string.h>

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
    atomic_int first_notice_entered, release_first_notice;
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
        /* Hold one supervisor CONTROL callback on its own actor lane.
         * A second terminal envelope may queue, but a third must get
         * explicit backpressure rather than overwriting the reserved slot. */
        atomic_store_explicit(&s->first_notice_entered, 1, memory_order_release);
        for (int i = 0; i < 5000 &&
                !atomic_load_explicit(&s->release_first_notice, memory_order_acquire); ++i) {
            const struct timespec nap = {0, 1000000};
            nanosleep(&nap, NULL);
        }
        CHECK(atomic_load_explicit(&s->release_first_notice, memory_order_acquire));
    } else if (observed == 1) {
        CHECK(f.kind == ORES_NATIVE_CONTROL_STOPPED);
        CHECK(f.child.scheduler_domain == s->replacement.scheduler_domain);
        CHECK(f.child.actor_slot == s->replacement.actor_slot);
        CHECK(f.child.generation == s->replacement.generation);
        CHECK(f.failure_code == 0);
        CHECK(f.correlation == 702);
    } else if (observed == 2) {
        CHECK(f.kind == ORES_NATIVE_CONTROL_STOPPED);
        CHECK(f.child.scheduler_domain == s->sibling.scheduler_domain);
        CHECK(f.child.actor_slot == s->sibling.actor_slot);
        CHECK(f.child.generation == s->sibling.generation);
        CHECK(f.failure_code == 0);
        CHECK(f.correlation == 703);
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
typedef struct {
    atomic_uint_fast64_t child_calls, sibling_calls, notices;
    atomic_int notice_entered, release_notice;
} ReuseState;
static void reuse_data(void* raw, size_t slot, uint64_t payload) {
    ReuseState* s = raw;
    CHECK((slot == 1 && payload == 7) || (slot == 2 && payload == 42));
    atomic_fetch_add(slot == 1 ? &s->child_calls : &s->sibling_calls, 1);
}
static void reuse_notice(void* raw, size_t parent, OresNativeActorTerminalFailure f) {
    ReuseState* s = raw;
    CHECK(parent == 0 && (f.child.actor_slot == 1 || f.child.actor_slot == 2));
    CHECK(f.kind == ORES_NATIVE_CONTROL_STOPPED && f.failure_code == 0);
    if (f.child.actor_slot == 1 && f.child.generation == 1) {
        atomic_store(&s->notice_entered, 1);
        while (!atomic_load(&s->release_notice)) sched_yield();
    }
    atomic_fetch_add(&s->notices, 1);
}
static void reclaim_after_ack(OresNativeVM* vm, OresNativeActorRef a) {
    struct timespec begin, now;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &begin) == 0);
    while (!ores_native_vm_reclaim_supervised_leaf_v1(vm, ORES_NATIVE_VM_MAIN, a)) {
        sched_yield();
        CHECK(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
        CHECK(now.tv_sec - begin.tv_sec < 15);
    }
}
static int million_reuse(void) {
    ReuseState s = {0};
    OresNativeVMConfig cfg = {0};
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i) {
        cfg.domains[i].actor_slots = i == ORES_NATIVE_VM_MAIN ? 3 : 1;
        cfg.domains[i].mailbox_capacity = 4;
        cfg.domains[i].carrier_workers = i == ORES_NATIVE_VM_MAIN ? 2 : 1;
        cfg.domains[i].on_message = reuse_data;
        cfg.domains[i].on_failure = reuse_notice;
        cfg.domains[i].context = &s;
    }
    OresNativeVM* vm = ores_native_vm_open(&cfg);
    CHECK(vm);
    const OresNativeActorRef root = ores_native_vm_spawn(vm, ORES_NATIVE_VM_MAIN, 0);
    const OresNativeActorRef sibling = ores_native_vm_spawn_supervised(vm, ORES_NATIVE_VM_MAIN, 2, root);
    CHECK(root.scheduler_domain && sibling.scheduler_domain);
    OresNativeActorRef stale = {0};
    for (uint64_t i = 0; i < UINT64_C(1000000); ++i) {
        OresNativeActorRef a = ores_native_vm_spawn_supervised(vm, ORES_NATIVE_VM_MAIN, 1, root);
        CHECK(a.scheduler_domain == root.scheduler_domain && a.actor_slot == 1 && a.generation == i + 1);
        CHECK(!ores_native_vm_reclaim_supervised_leaf_v1(vm, ORES_NATIVE_VM_MAIN, a));
        if (i) {
            CHECK(ores_native_vm_try_send(vm, ORES_NATIVE_VM_MAIN, stale, 7) == ORES_NATIVE_SEND_INVALID_REF);
            CHECK(!ores_native_vm_actor_resource_retain(vm, ORES_NATIVE_VM_MAIN, stale));
            CHECK(!ores_native_vm_actor_resource_release(vm, ORES_NATIVE_VM_MAIN, stale));
            CHECK(!ores_native_vm_confirm_supervised_finalized(vm, ORES_NATIVE_VM_MAIN, stale));
            CHECK(!ores_native_vm_reclaim_supervised_leaf_v1(vm, ORES_NATIVE_VM_MAIN, stale));
            CHECK(!ores_native_vm_stop(vm, ORES_NATIVE_VM_MAIN, stale));
        }
        CHECK(ores_native_vm_actor_resource_retain(vm, ORES_NATIVE_VM_MAIN, a));
        CHECK(ores_native_vm_try_send(vm, ORES_NATIVE_VM_MAIN, a, 7) == ORES_NATIVE_SEND_ACCEPTED);
        CHECK(ores_native_vm_stop(vm, ORES_NATIVE_VM_MAIN, a));
        CHECK(!ores_native_vm_poll_done(vm, ORES_NATIVE_VM_MAIN, a));
        CHECK(!ores_native_vm_confirm_supervised_finalized(vm, ORES_NATIVE_VM_MAIN, a));
        CHECK(!ores_native_vm_reclaim_supervised_leaf_v1(vm, ORES_NATIVE_VM_MAIN, a));
        CHECK(ores_native_vm_actor_resource_release(vm, ORES_NATIVE_VM_MAIN, a));
        CHECK(ores_native_vm_wait_done(vm, ORES_NATIVE_VM_MAIN, a));
        CHECK(atomic_load(&s.child_calls) == i + 1);
        CHECK(!ores_native_vm_reclaim_supervised_leaf_v1(vm, ORES_NATIVE_VM_MAIN, a));
        CHECK(ores_native_vm_confirm_supervised_finalized(vm, ORES_NATIVE_VM_MAIN, a));
        CHECK(!ores_native_vm_reclaim_supervised_leaf_v1(vm, ORES_NATIVE_VM_MAIN, a));
        OresNativeActorTerminalFailure notice = {
            .kind = ORES_NATIVE_CONTROL_STOPPED, .child = a, .correlation = i + 1
        };
        CHECK(ores_native_vm_publish_supervised_failure(vm, ORES_NATIVE_VM_MAIN, notice));
        if (!i) {
            await_one(&s.notice_entered);
            CHECK(!ores_native_vm_reclaim_supervised_leaf_v1(vm, ORES_NATIVE_VM_MAIN, a));
            CHECK(!ores_native_vm_spawn_supervised(vm, ORES_NATIVE_VM_MAIN, 1, root).scheduler_domain);
            atomic_store(&s.release_notice, 1);
        }
        reclaim_after_ack(vm, a);
        CHECK(atomic_load(&s.notices) == i + 1);
        CHECK(!ores_native_vm_publish_supervised_failure(vm, ORES_NATIVE_VM_MAIN, notice));
        CHECK(!ores_native_vm_poll_done(vm, ORES_NATIVE_VM_MAIN, a));
        CHECK(!ores_native_vm_supervisor_of(vm, ORES_NATIVE_VM_MAIN, a).scheduler_domain);
        CHECK(ores_native_vm_try_send(vm, ORES_NATIVE_VM_MAIN, a, 7) == ORES_NATIVE_SEND_INVALID_REF);
        /* Keep one live sibling throughout all slot incarnations. Probe its
         * real mailbox every cycle; permit bounded backpressure, not loss. */
        OresNativeSendStatus admitted;
        do {
            admitted = ores_native_vm_try_send(vm, ORES_NATIVE_VM_MAIN, sibling, 42);
            if (admitted == ORES_NATIVE_SEND_MAILBOX_FULL) sched_yield();
        } while (admitted == ORES_NATIVE_SEND_MAILBOX_FULL);
        CHECK(admitted == ORES_NATIVE_SEND_ACCEPTED);
        stale = a;
    }
    CHECK(ores_native_vm_stop(vm, ORES_NATIVE_VM_MAIN, sibling));
    CHECK(ores_native_vm_wait_done(vm, ORES_NATIVE_VM_MAIN, sibling));
    CHECK(atomic_load(&s.sibling_calls) == UINT64_C(1000000));
    CHECK(ores_native_vm_confirm_supervised_finalized(vm, ORES_NATIVE_VM_MAIN, sibling));
    OresNativeActorTerminalFailure last = {
        .kind = ORES_NATIVE_CONTROL_STOPPED, .child = sibling, .correlation = 1
    };
    CHECK(ores_native_vm_publish_supervised_failure(vm, ORES_NATIVE_VM_MAIN, last));
    reclaim_after_ack(vm, sibling);
    CHECK(atomic_load(&s.notices) == UINT64_C(1000001));
    CHECK(ores_native_vm_stop(vm, ORES_NATIVE_VM_MAIN, root));
    CHECK(ores_native_vm_wait_done(vm, ORES_NATIVE_VM_MAIN, root));
    CHECK(ores_native_vm_close(vm));
    puts("NATIVE_VM_LEAF_REUSE_PASS cycles=1000000 slots=3 sibling=1000000 stale=0 notices=1000001");
    return 0;
}
int main(int argc, char** argv) {
    if (argc == 2 && strcmp(argv[1], "--reuse") == 0) return million_reuse();
    CHECK(argc == 1);
    State s = {0};
    atomic_init(&s.failed_a, 0);
    atomic_init(&s.b_42, 0);
    atomic_init(&s.new_a_ok, 0);
    atomic_init(&s.notices, 0);
    atomic_init(&s.first_notice_entered, 0);
    atomic_init(&s.release_first_notice, 0);
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
    s.sibling = ores_native_vm_spawn_supervised(vm, ORES_NATIVE_VM_MAIN, ACTOR_B, s.parent);
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
    await_one(&s.first_notice_entered);

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
    /* Main preallocates one terminal CONTROL receipt per bound child, so a
     * third finalized child is admitted even while the first supervisor
     * CONTROL callback is still executing; parent CONTROL owns delivery.
     * A duplicate publish of the same receipt is still rejected. */
    CHECK(ores_native_vm_stop(vm, ORES_NATIVE_VM_MAIN, s.sibling));
    CHECK(ores_native_vm_wait_done(vm, ORES_NATIVE_VM_MAIN, s.sibling));
    CHECK(ores_logical_frames_v1_close(s.b));
    CHECK(ores_native_vm_confirm_supervised_finalized(vm, ORES_NATIVE_VM_MAIN, s.sibling));
    OresNativeActorTerminalFailure third = {
        .kind = ORES_NATIVE_CONTROL_STOPPED, .child = s.sibling,
        .failure_code = 0, .correlation = 703
    };
    CHECK(ores_native_vm_publish_supervised_failure(vm, ORES_NATIVE_VM_MAIN, third));
    atomic_store_explicit(&s.release_first_notice, 1, memory_order_release);
    for (int i = 0; i < 5000 && atomic_load_explicit(&s.notices, memory_order_acquire) < 2; ++i) {
        const struct timespec nap = {0, 1000000};
        nanosleep(&nap, NULL);
    }
    CHECK(atomic_load_explicit(&s.notices, memory_order_acquire) >= 2);
    CHECK(!ores_native_vm_publish_supervised_failure(vm, ORES_NATIVE_VM_MAIN, third));
    for (int i = 0; i < 5000 && atomic_load_explicit(&s.notices, memory_order_acquire) < 3; ++i) {
        const struct timespec nap = {0, 1000000};
        nanosleep(&nap, NULL);
    }
    CHECK(atomic_load_explicit(&s.notices, memory_order_acquire) == 3);
    CHECK(ores_native_vm_stop(vm, ORES_NATIVE_VM_MAIN, s.parent));
    CHECK(ores_native_vm_wait_done(vm, ORES_NATIVE_VM_MAIN, s.parent));
    CHECK(ores_native_vm_close(vm));
    puts("NATIVE_VM_SUPERVISED_QUOTA_PASS finalized=1 notices=3 B=42 replacement=1 backpressure_retry=1");
    return 0;
}
