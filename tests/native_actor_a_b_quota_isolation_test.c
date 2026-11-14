/* Source-free native actor-owned logical frame failure isolation regression.
 * Do not claim this proves compiler-generated guest actor failure routing.
 */
#include "oreslang/runtime/NativeActorLogicalFramesV1.h"
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "actor A/B frame quota check failed at %d: %s\n", __LINE__, #expr); \
    abort(); } } while (0)

int main(void) {
    OresNativeActorLogicalFramesV1* a = ores_logical_frames_v1_open(101, 1, 1, 1, 8);
    OresNativeActorLogicalFramesV1* b = ores_logical_frames_v1_open(102, 1, 1, 1, 8);
    CHECK(a && b);
    OresLogicalTurnV1 turn_a = {0}, turn_b = {0};
    OresLogicalFrameV1 frame_a = {0}, over = {0}, frame_b = {0};
    CHECK(ores_logical_frames_v1_begin(a, 101, 1, &turn_a));
    CHECK(ores_logical_frames_v1_push(a, turn_a, &frame_a));
    /* A hits its one-frame budget; failure is a local status, not process abort. */
    CHECK(!ores_logical_frames_v1_push(a, turn_a, &over));
    CHECK(!ores_logical_frames_v1_end(a, turn_a));
    /* B is independent and must still be able to execute a frame returning 42. */
    CHECK(ores_logical_frames_v1_begin(b, 102, 1, &turn_b));
    CHECK(ores_logical_frames_v1_push(b, turn_b, &frame_b));
    int64_t forty_two = 42, observed = 0;
    CHECK(ores_logical_frames_v1_write(b, frame_b, 0, &forty_two, sizeof forty_two));
    CHECK(ores_logical_frames_v1_read(b, frame_b, 0, &observed, sizeof observed));
    CHECK(observed == 42);
    CHECK(!ores_logical_frames_v1_read(b, frame_a, 0, &observed, sizeof observed));
    CHECK(ores_logical_frames_v1_pop(b, frame_b));
    CHECK(ores_logical_frames_v1_end(b, turn_b));
    /* Cancel A: wipe all frame bytes and reject any pre-cancel handle. */
    CHECK(ores_logical_frames_v1_cancel(a, turn_a));
    CHECK(!ores_logical_frames_v1_read(a, frame_a, 0, &observed, sizeof observed));
    CHECK(ores_logical_frames_v1_close(a));
    /* A's incarnation 2 must not accept incarnation 1 capability tokens. */
    OresNativeActorLogicalFramesV1* restarted = ores_logical_frames_v1_open(101, 2, 1, 1, 8);
    CHECK(restarted);
    OresLogicalTurnV1 new_turn = {0};
    OresLogicalFrameV1 new_frame = {0};
    CHECK(!ores_logical_frames_v1_begin(restarted, 101, 1, &new_turn));
    CHECK(ores_logical_frames_v1_begin(restarted, 101, 2, &new_turn));
    CHECK(!ores_logical_frames_v1_push(restarted, turn_a, &over));
    CHECK(ores_logical_frames_v1_push(restarted, new_turn, &new_frame));
    CHECK(!ores_logical_frames_v1_read(restarted, frame_a, 0, &observed, sizeof observed));
    CHECK(ores_logical_frames_v1_read(restarted, new_frame, 0, &observed, sizeof observed));
    CHECK(observed == 0); /* full byte scrubbing before frame reuse */
    CHECK(ores_logical_frames_v1_pop(restarted, new_frame));
    CHECK(ores_logical_frames_v1_end(restarted, new_turn));
    CHECK(ores_logical_frames_v1_close(restarted));
    CHECK(ores_logical_frames_v1_close(b));
    puts("native actors: A frame quota rejected, B returns 42, restart generation fenced");
    return 0;
}
