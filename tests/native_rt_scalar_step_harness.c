#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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
    assert(pid >= 0);
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
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFSIGNALED(status));
    assert(WTERMSIG(status) == SIGILL);
}
#endif

int main(void) {
    // Test real, independent HEAP frame ownership; no C/LLVM stack frame
    // survives an explicit guest yield. Host owns dispatch serially.
    ScalarFrame *a = calloc(1, sizeof(*a));
    ScalarFrame *b = calloc(1, sizeof(*b));
    ScalarFrame *cancelled = calloc(1, sizeof(*cancelled));
    assert(a && b && cancelled);
    oreslang_rt_scalar_frame_init(a);
    oreslang_rt_scalar_frame_init(b);
    oreslang_rt_scalar_frame_init(cancelled);
    assert(oreslang_rt_scalar_frame_step(a, 0) == 1);
    assert(a->counter == 0 && a->state == 1);
    assert(oreslang_rt_scalar_frame_step(a, 1) == 1);
    assert(a->counter == 1 && a->state == 1);
    // Exactly 512 backedges per dispatch, no replay on handoff.
    assert(oreslang_rt_scalar_frame_step(b, 512) == 1);
    assert(b->counter == 512);
    assert(oreslang_rt_scalar_frame_step(a, 512) == 1);
    assert(a->counter == 513);
    // A separate task cancellation must not mutate or replay other frames.
    assert(oreslang_rt_scalar_frame_step(cancelled, 9) == 1);
    assert(cancelled->counter == 9);
    oreslang_rt_scalar_frame_cancel(cancelled);
    assert(oreslang_rt_scalar_frame_step(cancelled, 512) == 2);
    assert(cancelled->counter == 9 && cancelled->state == 3);
    oreslang_rt_scalar_frame_cancel(cancelled);
    assert(oreslang_rt_scalar_frame_step(cancelled, 1) == 2);

    uint64_t prevA = a->counter, prevB = b->counter;
    unsigned rounds = 0;
    while (a->state != 2 || b->state != 2) {
        int sa = oreslang_rt_scalar_frame_step(a, 7);
        int sb = oreslang_rt_scalar_frame_step(b, 11);
        assert(sa == 0 || sa == 1);
        assert(sb == 0 || sb == 1);
        assert(a->counter >= prevA && a->counter <= prevA + 7);
        assert(b->counter >= prevB && b->counter <= prevB + 11);
        prevA = a->counter;
        prevB = b->counter;
        assert(++rounds < 300);
    }
    assert(oreslang_rt_scalar_frame_result(a) == 1000);
    assert(oreslang_rt_scalar_frame_result(b) == 1000);
    assert(oreslang_rt_scalar_frame_step(a, 512) == 0);
    assert(a->counter == 1000);
#ifdef __linux__
    for (int scenario = 0; scenario != 8; ++scenario)
        require_guest_trap(scenario);
#endif
    puts("SCALAR_FRAME_AOT_TWO_TASKS_PASS");
    free(cancelled);
    free(b);
    free(a);
    return 0;
}
