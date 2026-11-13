/* Real native execution of compiler-generated Atto Task producer *only*.
 * This is not an actor task, a scheduler test or an await conformance test.
 * Call it only with the LLVM IR emitted by --emit-actor-task-producer-ir-only.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>

extern int32_t atto_actor_task_i64_producer_probe(
    int64_t copied_incoming, int64_t* result_out);
extern int32_t atto_actor_task_i64_resume_probe(
    int64_t awaited_result, int64_t* resumed_result_out);
/* Compiler-defined six-word persistent frame: host must provide zeroed,
 * actor-owned storage. A probe frame is NOT a Task scheduler lease. */
extern int32_t atto_actor_task_i64_frame_init_probe(
    int64_t words[6], uint64_t actor_generation,
    uint64_t task_generation, int64_t copied_incoming);
extern int32_t atto_actor_task_i64_frame_step_probe(
    int64_t words[6], uint64_t actor_generation, uint64_t task_generation);
extern int32_t atto_actor_task_i64_frame_resume_probe(
    int64_t words[6], uint64_t actor_generation,
    uint64_t task_generation, int64_t* result_out);

int main(int argc, char** argv) {
    if (argc != 2) return 10;
    char* end = NULL;
    const long expected = strtol(argv[1], &end, 10);
    if (!end || *end || expected < INT32_MIN || expected > INT32_MAX) return 11;
    int64_t result = INT64_C(987654321);
    if (atto_actor_task_i64_producer_probe(INT64_C(41), &result) != 0 ||
            result != (int64_t)expected) return 12;
    int64_t resumed = INT64_C(-7);
    if (atto_actor_task_i64_resume_probe(result, &resumed) != 0 ||
            resumed != (int64_t)expected) return 15;
    if (atto_actor_task_i64_resume_probe(result, NULL) != 2) return 16;
    /* No native trap or wraparound on signed overflow. Rejected output must
     * remain untouched, preserving the caller's committed actor state. */
    result = INT64_C(987654321);
    if (atto_actor_task_i64_producer_probe(INT64_MAX, &result) != 1 ||
            result != INT64_C(987654321)) return 13;
    if (atto_actor_task_i64_producer_probe(INT64_C(41), NULL) != 2) return 14;

    /* The source compiler also emits a persistent, checked frame state
     * machine. No future callback may bypass actor-generation and task-id
     * fences, replay a completed step, or reinitialize a retired frame. */
    int64_t frame[6] = {0};
    if (atto_actor_task_i64_frame_init_probe(
            NULL, 11, 1002, 41) != 2) return 20;
    if (atto_actor_task_i64_frame_init_probe(
            frame, 0, 1002, 41) != 3) return 21;
    if (atto_actor_task_i64_frame_init_probe(
            frame, 11, 0, 41) != 3) return 22;
    if (atto_actor_task_i64_frame_init_probe(
            frame, 11, 1002, 41) != 0 || frame[4] != 1) return 23;
    if (atto_actor_task_i64_frame_init_probe(
            frame, 11, 1002, 41) != 4) return 24;
    int64_t premature = INT64_C(-99);
    if (atto_actor_task_i64_frame_resume_probe(
            frame, 11, 1002, &premature) != 4 ||
            premature != -99 || frame[4] != 1) return 40;
    if (atto_actor_task_i64_frame_step_probe(
            frame, 12, 1002) != 3 || frame[4] != 1) return 25;
    if (atto_actor_task_i64_frame_step_probe(
            frame, 11, 1003) != 3 || frame[4] != 1) return 26;
    if (atto_actor_task_i64_frame_step_probe(
            NULL, 11, 1002) != 2) return 27;
    if (atto_actor_task_i64_frame_step_probe(
            frame, 11, 1002) != 0 ||
            frame[3] != (int64_t)expected || frame[4] != 2) return 28;
    if (atto_actor_task_i64_frame_step_probe(
            frame, 11, 1002) != 4) return 29;
    int64_t guarded_output = INT64_C(-99);
    if (atto_actor_task_i64_frame_resume_probe(
            frame, 12, 1002, &guarded_output) != 3 ||
            guarded_output != -99 || frame[4] != 2) return 30;
    if (atto_actor_task_i64_frame_resume_probe(
            frame, 11, 1003, &guarded_output) != 3 ||
            guarded_output != -99 || frame[4] != 2) return 31;
    if (atto_actor_task_i64_frame_resume_probe(
            frame, 11, 1002, NULL) != 2 || frame[4] != 2) return 32;
    if (atto_actor_task_i64_frame_resume_probe(
            frame, 11, 1002, &guarded_output) != 0 ||
            guarded_output != (int64_t)expected || frame[4] != 3) return 33;
    guarded_output = INT64_C(-99);
    if (atto_actor_task_i64_frame_resume_probe(
            frame, 11, 1002, &guarded_output) != 4 ||
            guarded_output != -99) return 34;
    if (atto_actor_task_i64_frame_init_probe(
            frame, 11, 1002, 41) != 4) return 35;

    int64_t overflow_frame[6] = {0};
    if (atto_actor_task_i64_frame_init_probe(
            overflow_frame, 11, 1004, INT64_MAX) != 0) return 36;
    if (atto_actor_task_i64_frame_step_probe(
            overflow_frame, 11, 1004) != 1 ||
            overflow_frame[4] != 4 || overflow_frame[3] != 0) return 37;
    guarded_output = INT64_C(-99);
    if (atto_actor_task_i64_frame_resume_probe(
            overflow_frame, 11, 1004, &guarded_output) != 4 ||
            guarded_output != -99) return 38;
    overflow_frame[5] = 0; /* tampered ABI marker is denied. */
    if (atto_actor_task_i64_frame_step_probe(
            overflow_frame, 11, 1004) != 3) return 39;
    puts("ATTO_TASK_I64_PRODUCER_IR_ONLY_PASS");
    return 0;
}
