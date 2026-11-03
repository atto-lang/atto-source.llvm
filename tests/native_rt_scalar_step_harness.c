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
#ifdef __linux__
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

#ifdef __linux__
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
        default: _Exit(99);
        }
        _Exit(0); /* An invalid operation returning constitutes a failure. */
    }
    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(WIFSIGNALED(status));
    CHECK(WTERMSIG(status) == SIGILL);
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
#ifdef __linux__
    for (int scenario = 0; scenario != 8; ++scenario)
        require_guest_trap(scenario);
#endif
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
