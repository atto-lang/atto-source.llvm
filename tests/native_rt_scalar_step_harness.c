#if defined(__linux__) && !defined(_POSIX_C_SOURCE)
/* fork/waitpid are POSIX, not ISO C11: declare them under -std=c11 -pedantic. */
#define _POSIX_C_SOURCE 200809L
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
/* Keep every regression check live under -DNDEBUG / Release optimization. */
#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "frame check failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        abort(); \
    } \
} while (0)
#if defined(__linux__) || defined(__APPLE__)
#define ORESLANG_FRAME_TRAP_PROOF 1
#endif
#ifdef ORESLANG_FRAME_TRAP_PROOF
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

extern void oreslang_rt_scalar_frame_init(void *);
extern int oreslang_rt_scalar_frame_step(void *, uint64_t);
extern void oreslang_rt_scalar_frame_cancel(void *);
extern int64_t oreslang_rt_scalar_frame_result(void *);

typedef struct {
    uint64_t state;
    uint64_t counter;
    uint64_t magic;
} ScalarFrame;

#ifdef ORESLANG_FRAME_TRAP_PROOF
/* Real llvm.trap must fail closed in a child, not silently return a code or
 * produce partially usable data. Exercise one violation per process because
 * production treats all invalid task-frame operations as fatal. */
static void require_guest_trap(int scenario) {
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        ScalarFrame frame = {0};
        switch (scenario) {
        case 0: (void)oreslang_rt_scalar_frame_step(&frame, 1); break;
        case 1: oreslang_rt_scalar_frame_cancel(&frame); break;
        case 2: oreslang_rt_scalar_frame_init(&frame);
                oreslang_rt_scalar_frame_init(&frame); break;
        case 3: oreslang_rt_scalar_frame_init(&frame);
                (void)oreslang_rt_scalar_frame_step(&frame, 513); break;
        case 4: oreslang_rt_scalar_frame_init(&frame);
                frame.magic = 0;
                (void)oreslang_rt_scalar_frame_step(&frame, 1); break;
        case 5: oreslang_rt_scalar_frame_init(&frame);
                (void)oreslang_rt_scalar_frame_result(&frame); break;
        case 6: oreslang_rt_scalar_frame_init(&frame);
                frame.state = 99;
                (void)oreslang_rt_scalar_frame_step(&frame, 1); break;
        case 7: (void)oreslang_rt_scalar_frame_step(NULL, 1); break;
        case 8: oreslang_rt_scalar_frame_init(&frame);
                frame.counter = UINT64_MAX; /* signed -1: before source initial */
                (void)oreslang_rt_scalar_frame_step(&frame, 1); break;
        case 9: oreslang_rt_scalar_frame_init(&frame);
                frame.counter = 1001; /* past source's exclusive bound */
                (void)oreslang_rt_scalar_frame_step(&frame, 1); break;
        /* Dirty-frame zero-init: state==0 alone is not a fresh frame. */
        case 10: frame.counter = 1; /* dirty counter, zero state */
                 oreslang_rt_scalar_frame_init(&frame); break;
        case 11: frame.magic = 7; /* dirty magic cannot resurrect a frame */
                 oreslang_rt_scalar_frame_init(&frame); break;
        /* Forged DONE: the scheduler may consume status 0 without result(),
         * so step, cancel AND result must all verify the committed counter. */
        case 12: oreslang_rt_scalar_frame_init(&frame);
                 frame.state = 2; frame.counter = 999;
                 (void)oreslang_rt_scalar_frame_step(&frame, 1); break;
        case 13: oreslang_rt_scalar_frame_init(&frame);
                 frame.state = 2; frame.counter = 999;
                 (void)oreslang_rt_scalar_frame_step(&frame, 0); break;
        case 14: oreslang_rt_scalar_frame_init(&frame);
                 frame.state = 2; frame.counter = 999;
                 oreslang_rt_scalar_frame_cancel(&frame); break;
        case 15: oreslang_rt_scalar_frame_init(&frame);
                 frame.state = 2; frame.counter = 999;
                 (void)oreslang_rt_scalar_frame_result(&frame); break;
        case 16: oreslang_rt_scalar_frame_init(&frame);
                 frame.state = 2; frame.counter = 1001; /* overshot terminal */
                 (void)oreslang_rt_scalar_frame_result(&frame); break;
        /* Corrupt committed counters in non-active states / cancellation. */
        case 17: oreslang_rt_scalar_frame_init(&frame);
                 frame.counter = 1001;
                 oreslang_rt_scalar_frame_cancel(&frame); break;
        case 18: oreslang_rt_scalar_frame_init(&frame);
                 oreslang_rt_scalar_frame_cancel(&frame);
                 frame.counter = UINT64_MAX; /* corrupt CANCELLED frame */
                 (void)oreslang_rt_scalar_frame_step(&frame, 1); break;
        /* Cancellation is distinct from completion: no result is published. */
        case 19: oreslang_rt_scalar_frame_init(&frame);
                 (void)oreslang_rt_scalar_frame_step(&frame, 64);
                 oreslang_rt_scalar_frame_cancel(&frame);
                 (void)oreslang_rt_scalar_frame_result(&frame); break;
        default: _Exit(99);
        }
        _Exit(0); /* An invalid operation returning constitutes a failure. */
    }
    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(WIFSIGNALED(status));
    /* llvm.trap is ud2 (SIGILL) on x86-64 and brk (SIGTRAP) on arm64. */
    CHECK(WTERMSIG(status) == SIGILL || WTERMSIG(status) == SIGTRAP);
}
#endif

/*
 * Run three separately initialized frames through the REAL generated step ABI.
 * A 7/11/13 reduction quantum deliberately interleaves them without sharing
 * stack frames, counters, or a mutable source continuation.  This is a host
 * dispatch oracle: it proves the generated continuation *permits* fair
 * scheduling, not that OresScheduler/native actors supply this policy yet.
 */
static void require_bounded_peer_progress(void) {
    static const uint64_t fuel[3] = {7, 11, 13};
    ScalarFrame *frames[3] = {0};
    for (unsigned i = 0; i < 3; ++i) {
        frames[i] = calloc(1, sizeof(*frames[i]));
        CHECK(frames[i] != NULL);
        oreslang_rt_scalar_frame_init(frames[i]);
        CHECK(frames[i]->counter == 0 && frames[i]->state == 1);
    }
    uint64_t dispatches[3] = {0, 0, 0};
    unsigned rounds = 0;
    for (;;) {
        unsigned retired = 0;
        for (unsigned i = 0; i < 3; ++i) {
            const uint64_t before = frames[i]->counter;
            const uint64_t expected_advance =
                before < 1000 && fuel[i] < 1000 - before
                    ? fuel[i] : (before < 1000 ? 1000 - before : 0);
            const int result = oreslang_rt_scalar_frame_step(frames[i], fuel[i]);
            ++dispatches[i];
            CHECK(result == 0 || result == 1);
            CHECK(frames[i]->counter == before + expected_advance);
            CHECK(frames[i]->counter <= 1000);
            if (result == 0) {
                CHECK(frames[i]->state == 2);
                CHECK(oreslang_rt_scalar_frame_result(frames[i]) == 1000);
                ++retired;
            } else {
                CHECK(frames[i]->state == 1);
            }
        }
        // Each peer received one bounded dispatch before any is revisited;
        // none can monopolize the host carrier with an infinite guest loop.
        CHECK(dispatches[0] == dispatches[1]);
        CHECK(dispatches[1] == dispatches[2]);
        CHECK(++rounds <= 145);
        if (retired == 3) break;
    }
    CHECK(dispatches[0] == 143); /* ceil(1000 / 7) */
    CHECK(dispatches[1] == 143);
    CHECK(dispatches[2] == 143);
    for (unsigned i = 0; i < 3; ++i) {
        CHECK(oreslang_rt_scalar_frame_step(frames[i], 0) == 0);
        CHECK(oreslang_rt_scalar_frame_result(frames[i]) == 1000);
        free(frames[i]); // after terminal guest dispatch has returned
    }
}

/*
 * In a mixed scheduler cohort, retiring a cancelled source continuation
 * must not mutate another frame or halt its bounded forward progress.
 * This is deliberately executed through the compiled guest ABI.
 */
static void require_cancelled_peer_is_isolated(void) {
    ScalarFrame *cancel = calloc(1, sizeof(ScalarFrame));
    ScalarFrame *survive_a = calloc(1, sizeof(ScalarFrame));
    ScalarFrame *survive_b = calloc(1, sizeof(ScalarFrame));
    CHECK(cancel && survive_a && survive_b);
    oreslang_rt_scalar_frame_init(cancel);
    oreslang_rt_scalar_frame_init(survive_a);
    oreslang_rt_scalar_frame_init(survive_b);
    CHECK(oreslang_rt_scalar_frame_step(cancel, 64) == 1);
    CHECK(cancel->counter == 64);
    CHECK(oreslang_rt_scalar_frame_step(survive_a, 31) == 1);
    CHECK(oreslang_rt_scalar_frame_step(survive_b, 29) == 1);
    const uint64_t before_a = survive_a->counter;
    const uint64_t before_b = survive_b->counter;
    oreslang_rt_scalar_frame_cancel(cancel);
    CHECK(oreslang_rt_scalar_frame_step(cancel, 7) == 2);
    CHECK(oreslang_rt_scalar_frame_step(cancel, 512) == 2);
    CHECK(cancel->counter == 64 && cancel->state == 3);
    CHECK(survive_a->counter == before_a && survive_b->counter == before_b);
    unsigned rounds = 0;
    while (survive_a->state != 2 || survive_b->state != 2) {
        const uint64_t a_before = survive_a->counter;
        const uint64_t b_before = survive_b->counter;
        const int sa = oreslang_rt_scalar_frame_step(survive_a, 31);
        const int sb = oreslang_rt_scalar_frame_step(survive_b, 29);
        CHECK(sa == 0 || sa == 1);
        CHECK(sb == 0 || sb == 1);
        CHECK(survive_a->counter >= a_before &&
              survive_a->counter <= a_before + 31);
        CHECK(survive_b->counter >= b_before &&
              survive_b->counter <= b_before + 29);
        CHECK(cancel->counter == 64 && cancel->state == 3);
        CHECK(++rounds <= 36);
    }
    CHECK(oreslang_rt_scalar_frame_result(survive_a) == 1000);
    CHECK(oreslang_rt_scalar_frame_result(survive_b) == 1000);
    free(survive_b);
    free(survive_a);
    free(cancel);
}

/* Exact per-dispatch fuel boundaries 0/1/127/128/129/511/512 against the
 * real generated step. #1569's batch endpoint min(counter + fuel, bound)
 * must commit exactly `fuel` effects from a fresh frame, and when a batch
 * reaches the terminal bound it must commit only what remains and return
 * DONE (never COOPERATE, never an overrun or a replay). */
static void require_exact_fuel_boundaries(void) {
    static const uint64_t cases[] = {0, 1, 127, 128, 129, 511, 512};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        const uint64_t fuel = cases[i];
        ScalarFrame *f = calloc(1, sizeof(*f));
        CHECK(f != NULL);
        oreslang_rt_scalar_frame_init(f);
        CHECK(oreslang_rt_scalar_frame_step(f, fuel) == 1);
        CHECK(f->state == 1 && f->counter == fuel);
        uint64_t before = fuel;
        int dispatches = 0;
        while (f->state != 2) {
            const uint64_t need = 1000 - before;
            const uint64_t advance = need < 512 ? need : 512;
            const int status = oreslang_rt_scalar_frame_step(f, 512);
            CHECK(f->counter == before + advance);
            CHECK(status == (f->counter == 1000 ? 0 : 1));
            CHECK(f->state == (f->counter == 1000 ? 2 : 1));
            before = f->counter;
            CHECK(++dispatches <= 2);
        }
        CHECK(oreslang_rt_scalar_frame_result(f) == 1000);
        free(f);
        if (fuel == 0)
            continue;
        /* Terminal-side boundary: start exactly `fuel` and `fuel + 1` short. */
        for (uint64_t shortfall = fuel; shortfall <= fuel + 1; ++shortfall) {
            ScalarFrame *t = calloc(1, sizeof(*t));
            CHECK(t != NULL);
            oreslang_rt_scalar_frame_init(t);
            uint64_t target = 1000 - shortfall;
            while (t->counter < target) {
                const uint64_t gap = target - t->counter;
                CHECK(oreslang_rt_scalar_frame_step(t, gap < 512 ? gap : 512) == 1);
            }
            CHECK(t->counter == target && t->state == 1);
            const int status = oreslang_rt_scalar_frame_step(t, fuel);
            if (shortfall == fuel) {
                CHECK(status == 0 && t->state == 2 && t->counter == 1000);
                CHECK(oreslang_rt_scalar_frame_result(t) == 1000);
            } else {
                CHECK(status == 1 && t->state == 1 && t->counter == 999);
            }
            free(t);
        }
    }
}

/* Exactly-once retirement: once DONE (or CANCELLED) a frame stays retired.
 * Repeated step/cancel never re-run, rewind or reclassify it. */
static void require_terminal_retirement_is_stable(void) {
    ScalarFrame *d = calloc(1, sizeof(*d));
    ScalarFrame *c = calloc(1, sizeof(*c));
    CHECK(d && c);
    oreslang_rt_scalar_frame_init(d);
    oreslang_rt_scalar_frame_init(c);
    CHECK(oreslang_rt_scalar_frame_step(d, 512) == 1);
    CHECK(oreslang_rt_scalar_frame_step(d, 512) == 0);
    CHECK(d->state == 2 && d->counter == 1000);
    for (int i = 0; i < 3; ++i) {
        oreslang_rt_scalar_frame_cancel(d); /* no-op: DONE is not CANCELLED */
        CHECK(d->state == 2 && d->counter == 1000);
        CHECK(oreslang_rt_scalar_frame_step(d, (uint64_t)(i * 256)) == 0);
        CHECK(oreslang_rt_scalar_frame_result(d) == 1000);
    }
    CHECK(oreslang_rt_scalar_frame_step(c, 64) == 1 && c->counter == 64);
    oreslang_rt_scalar_frame_cancel(c);
    for (int i = 0; i < 3; ++i) {
        /* CANCELLED is a distinct terminal status, never DONE/COOPERATE. */
        CHECK(oreslang_rt_scalar_frame_step(c, (uint64_t)(i * 256)) == 2);
        oreslang_rt_scalar_frame_cancel(c);
        CHECK(c->state == 3 && c->counter == 64);
    }
    free(c);
    free(d);
}

/* Real compiler-generated guest step: a short/long quantum must commit
 * exactly its permitted number of effects. In particular, boundary 127/128
 * and 511/512 never overrun, duplicate or erase the heap continuation. */
static void require_batch_fuel_boundary_matrix(void) {
    ScalarFrame *frame = calloc(1, sizeof(*frame));
    CHECK(frame != NULL);
    oreslang_rt_scalar_frame_init(frame);
    static const uint64_t fuel[] = {0, 1, 126, 1, 128, 255, 1, 0, 488};
    static const uint64_t after[] = {0, 1, 127, 128, 256, 511, 512, 512, 1000};
    for (size_t i = 0; i < sizeof(fuel) / sizeof(fuel[0]); ++i) {
        const int status = oreslang_rt_scalar_frame_step(frame, fuel[i]);
        CHECK(frame->counter == after[i]);
        CHECK(status == (after[i] == 1000 ? 0 : 1));
        CHECK(frame->state == (after[i] == 1000 ? 2 : 1));
    }
    CHECK(oreslang_rt_scalar_frame_result(frame) == 1000);
    CHECK(oreslang_rt_scalar_frame_step(frame, 0) == 0);
    CHECK(oreslang_rt_scalar_frame_step(frame, 512) == 0);
    CHECK(frame->counter == 1000 && frame->state == 2);
    free(frame);
}

int main(void) {
    // Test real, independent HEAP frame ownership; no C/LLVM stack frame
    // survives an explicit guest yield. Host owns dispatch serially.
    ScalarFrame *a = calloc(1, sizeof(*a));
    ScalarFrame *b = calloc(1, sizeof(*b));
    ScalarFrame *cancelled = calloc(1, sizeof(*cancelled));
    CHECK(a && b && cancelled);
    oreslang_rt_scalar_frame_init(a);
    oreslang_rt_scalar_frame_init(b);
    oreslang_rt_scalar_frame_init(cancelled);
    CHECK(oreslang_rt_scalar_frame_step(a, 0) == 1);
    CHECK(a->counter == 0 && a->state == 1);
    CHECK(oreslang_rt_scalar_frame_step(a, 1) == 1);
    CHECK(a->counter == 1 && a->state == 1);
    // Exactly 512 backedges per dispatch, no replay on handoff.
    CHECK(oreslang_rt_scalar_frame_step(b, 512) == 1);
    CHECK(b->counter == 512);
    CHECK(oreslang_rt_scalar_frame_step(a, 512) == 1);
    CHECK(a->counter == 513);
    // A separate task cancellation must not mutate or replay other frames.
    CHECK(oreslang_rt_scalar_frame_step(cancelled, 9) == 1);
    CHECK(cancelled->counter == 9);
    oreslang_rt_scalar_frame_cancel(cancelled);
    CHECK(oreslang_rt_scalar_frame_step(cancelled, 512) == 2);
    CHECK(cancelled->counter == 9 && cancelled->state == 3);
    oreslang_rt_scalar_frame_cancel(cancelled);
    CHECK(oreslang_rt_scalar_frame_step(cancelled, 1) == 2);

    uint64_t prevA = a->counter, prevB = b->counter;
    unsigned rounds = 0;
    while (a->state != 2 || b->state != 2) {
        int sa = oreslang_rt_scalar_frame_step(a, 7);
        int sb = oreslang_rt_scalar_frame_step(b, 11);
        CHECK(sa == 0 || sa == 1);
        CHECK(sb == 0 || sb == 1);
        CHECK(a->counter >= prevA && a->counter <= prevA + 7);
        CHECK(b->counter >= prevB && b->counter <= prevB + 11);
        prevA = a->counter;
        prevB = b->counter;
        CHECK(++rounds < 300);
    }
    CHECK(oreslang_rt_scalar_frame_result(a) == 1000);
    CHECK(oreslang_rt_scalar_frame_result(b) == 1000);
    CHECK(oreslang_rt_scalar_frame_step(a, 512) == 0);
    CHECK(a->counter == 1000);
#ifdef ORESLANG_FRAME_TRAP_PROOF
    for (int scenario = 0; scenario != 20; ++scenario)
        require_guest_trap(scenario);
    puts("SCALAR_FRAME_AOT_INVALID_FRAME_TRAPS_20_PASS");
#endif
    require_batch_fuel_boundary_matrix();
    require_exact_fuel_boundaries();
    puts("SCALAR_FRAME_AOT_BATCH_FUEL_BOUNDARIES_PASS");
    require_terminal_retirement_is_stable();
    puts("SCALAR_FRAME_AOT_TERMINAL_RETIREMENT_PASS");
    require_bounded_peer_progress();
    require_cancelled_peer_is_isolated();
    puts("SCALAR_FRAME_AOT_CANCELLED_PEER_ISOLATION_PASS");
    puts("SCALAR_FRAME_AOT_THREE_PEERS_BOUNDED_PROGRESS_PASS");
    puts("SCALAR_FRAME_AOT_TWO_TASKS_PASS");
    free(cancelled);
    free(b);
    free(a);
    return 0;
}
