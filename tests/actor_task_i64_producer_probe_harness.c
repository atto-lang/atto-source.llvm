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
    puts("ATTO_TASK_I64_PRODUCER_IR_ONLY_PASS");
    return 0;
}
