#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct OresNativeActorLogicalFramesV1 OresNativeActorLogicalFramesV1;
typedef struct {
    uint64_t owner, incarnation, domain;
    uint32_t slot;
    uint64_t generation;
} OresLogicalTurnV1;
typedef struct {
    OresLogicalTurnV1 turn;
    uint32_t depth;
    uint64_t generation;
} OresLogicalFrameV1;

/* Trusted host/AOT ABI, not a guest-visible raw-pointer capability.
 * All storage is allocated at startup and reclaimed deterministically.
 * A regular actor can have multiple simultaneous physical receiver turns;
 * serial actors must be configured with max_turns = 1.
 * max_turns <= 64, max_depth <= 256, frame_bytes <= 1024, total <= 16 MiB.
 */
OresNativeActorLogicalFramesV1* ores_logical_frames_v1_open(
    uint64_t owner, uint64_t incarnation, uint32_t max_turns,
    uint32_t max_depth, uint32_t frame_bytes);
int ores_logical_frames_v1_begin(OresNativeActorLogicalFramesV1*,
    uint64_t owner, uint64_t incarnation, OresLogicalTurnV1* out);
int ores_logical_frames_v1_push(OresNativeActorLogicalFramesV1*,
    OresLogicalTurnV1, OresLogicalFrameV1* out);
int ores_logical_frames_v1_read(OresNativeActorLogicalFramesV1*,
    OresLogicalFrameV1, uint32_t offset, void* out, uint32_t bytes);
int ores_logical_frames_v1_write(OresNativeActorLogicalFramesV1*,
    OresLogicalFrameV1, uint32_t offset, const void* data, uint32_t bytes);
int ores_logical_frames_v1_pop(OresNativeActorLogicalFramesV1*,
    OresLogicalFrameV1);
/* Normal exit requires depth==0. Cancel scrubs all frames on that turn and
 * invalidates all outstanding tokens, including after a quota failure. */
int ores_logical_frames_v1_end(OresNativeActorLogicalFramesV1*,
    OresLogicalTurnV1);
int ores_logical_frames_v1_cancel(OresNativeActorLogicalFramesV1*,
    OresLogicalTurnV1);
/* Close rejects outstanding turns; never free a buffer in use by another
 * carrier. After a successful close the pointer must not be reused. */
int ores_logical_frames_v1_close(OresNativeActorLogicalFramesV1*);
#ifdef __cplusplus
}
#endif
