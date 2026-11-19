#include "oreslang/runtime/NativeActorLogicalFramesV1.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TURNS 64u
#define MAX_DEPTH 256u
#define MAX_FRAME_BYTES 1024u
#define MAX_TOTAL_BYTES (16u * 1024u * 1024u)

typedef struct {
    pthread_t carrier;
    uint64_t generation;
    uint32_t depth;
    int active;
} TurnSlot;
struct OresNativeActorLogicalFramesV1 {
    pthread_mutex_t mutex;
    uint64_t owner, incarnation, domain;
    uint32_t max_turns, max_depth, frame_bytes;
    TurnSlot* turns;
    uint64_t* frame_generations;
    unsigned char* bytes;
};
static _Atomic uint64_t next_domain = 1;
static void wipe(void* addr, size_t n) {
    volatile unsigned char* p = (volatile unsigned char*)addr;
    while (n--) *p++ = 0;
}
static int lock(OresNativeActorLogicalFramesV1* s) {
    return s && pthread_mutex_lock(&s->mutex) == 0;
}
static void unlock(OresNativeActorLogicalFramesV1* s) {
    (void)pthread_mutex_unlock(&s->mutex);
}
static size_t index_of(const OresNativeActorLogicalFramesV1* s,
                       uint32_t slot, uint32_t depth) {
    return (size_t)slot * s->max_depth + depth;
}
static int valid_turn(const OresNativeActorLogicalFramesV1* s,
                      OresLogicalTurnV1 t) {
    if (t.slot >= s->max_turns || !t.generation || t.owner != s->owner ||
        t.incarnation != s->incarnation || t.domain != s->domain) return 0;
    const TurnSlot* slot = &s->turns[t.slot];
    return slot->active && slot->generation == t.generation &&
        pthread_equal(slot->carrier, pthread_self());
}
static int valid_frame(const OresNativeActorLogicalFramesV1* s,
                       OresLogicalFrameV1 f) {
    if (!valid_turn(s, f.turn) || !f.generation || f.depth >= s->max_depth ||
        f.depth >= s->turns[f.turn.slot].depth) return 0;
    return s->frame_generations[index_of(s, f.turn.slot, f.depth)] == f.generation;
}
OresNativeActorLogicalFramesV1* ores_logical_frames_v1_open(
        uint64_t owner, uint64_t incarnation, uint32_t turns,
        uint32_t depth, uint32_t bytes) {
    if (!owner || !incarnation || !turns || turns > MAX_TURNS ||
        !depth || depth > MAX_DEPTH || !bytes || bytes > MAX_FRAME_BYTES ||
        (uint64_t)turns * depth * bytes > MAX_TOTAL_BYTES) return NULL;
    OresNativeActorLogicalFramesV1* s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->owner = owner; s->incarnation = incarnation;
    s->domain = atomic_fetch_add_explicit(&next_domain, 1, memory_order_relaxed);
    if (!s->domain || s->domain == UINT64_MAX) { free(s); return NULL; }
    s->max_turns = turns; s->max_depth = depth; s->frame_bytes = bytes;
    s->turns = calloc(turns, sizeof(*s->turns));
    s->frame_generations = calloc((size_t)turns * depth, sizeof(uint64_t));
    s->bytes = calloc((size_t)turns * depth, bytes);
    if (!s->turns || !s->frame_generations || !s->bytes ||
        pthread_mutex_init(&s->mutex, NULL) != 0) {
        free(s->turns); free(s->frame_generations); free(s->bytes); free(s);
        return NULL;
    }
    return s;
}
int ores_logical_frames_v1_begin(OresNativeActorLogicalFramesV1* s,
        uint64_t owner, uint64_t incarnation, OresLogicalTurnV1* out) {
    if (!out || !lock(s)) return 0;
    int ok = 0;
    if (owner == s->owner && incarnation == s->incarnation) {
        /* No second active turn on this carrier: no accidental token alias. */
        int already = 0;
        for (uint32_t i = 0; i < s->max_turns; ++i)
            if (s->turns[i].active && pthread_equal(s->turns[i].carrier, pthread_self()))
                already = 1;
        if (!already) for (uint32_t i = 0; i < s->max_turns; ++i) {
            TurnSlot* t = &s->turns[i];
            if (t->active || t->generation == UINT64_MAX) continue;
            ++t->generation; t->active = 1; t->depth = 0;
            t->carrier = pthread_self();
            *out = (OresLogicalTurnV1){owner, incarnation, s->domain, i, t->generation};
            ok = 1; break;
        }
    }
    unlock(s); return ok;
}
int ores_logical_frames_v1_push(OresNativeActorLogicalFramesV1* s,
        OresLogicalTurnV1 t, OresLogicalFrameV1* out) {
    if (!out || !lock(s)) return 0;
    int ok = 0;
    if (valid_turn(s, t)) {
        TurnSlot* slot = &s->turns[t.slot];
        if (slot->depth < s->max_depth) {
            uint32_t depth = slot->depth;
            size_t i = index_of(s, t.slot, depth);
            if (s->frame_generations[i] != UINT64_MAX) {
                ++s->frame_generations[i];
                wipe(s->bytes + i * s->frame_bytes, s->frame_bytes);
                ++slot->depth;
                *out = (OresLogicalFrameV1){t, depth, s->frame_generations[i]};
                ok = 1;
            }
        }
    }
    unlock(s); return ok;
}
static int frame_access(OresNativeActorLogicalFramesV1* s,
        OresLogicalFrameV1 f, uint32_t offset, void* ptr, uint32_t bytes, int write) {
    if (!ptr || !lock(s)) return 0;
    int ok = valid_frame(s, f) && offset <= s->frame_bytes &&
        bytes <= s->frame_bytes - offset;
    if (ok) {
        unsigned char* at = s->bytes + index_of(s, f.turn.slot, f.depth) * s->frame_bytes + offset;
        if (write) memcpy(at, ptr, bytes); else memcpy(ptr, at, bytes);
    }
    unlock(s); return ok;
}
int ores_logical_frames_v1_read(OresNativeActorLogicalFramesV1* s,
        OresLogicalFrameV1 f, uint32_t offset, void* out, uint32_t bytes) {
    return frame_access(s, f, offset, out, bytes, 0);
}
int ores_logical_frames_v1_write(OresNativeActorLogicalFramesV1* s,
        OresLogicalFrameV1 f, uint32_t offset, const void* data, uint32_t bytes) {
    return frame_access(s, f, offset, (void*)data, bytes, 1);
}
int ores_logical_frames_v1_pop(OresNativeActorLogicalFramesV1* s,
        OresLogicalFrameV1 f) {
    if (!lock(s)) return 0;
    int ok = valid_frame(s, f) && s->turns[f.turn.slot].depth == f.depth + 1;
    if (ok) {
        wipe(s->bytes + index_of(s, f.turn.slot, f.depth) * s->frame_bytes, s->frame_bytes);
        --s->turns[f.turn.slot].depth;
    }
    unlock(s); return ok;
}
static int finish(OresNativeActorLogicalFramesV1* s, OresLogicalTurnV1 t,
                  int cancel) {
    if (!lock(s)) return 0;
    int ok = valid_turn(s, t) && (cancel || s->turns[t.slot].depth == 0);
    if (ok) {
        size_t start = index_of(s, t.slot, 0);
        wipe(s->bytes + start * s->frame_bytes,
             (size_t)s->max_depth * s->frame_bytes);
        s->turns[t.slot].depth = 0;
        s->turns[t.slot].active = 0;
        /* generation never resets, so stale handle cannot revalidate */
    }
    unlock(s); return ok;
}
int ores_logical_frames_v1_end(OresNativeActorLogicalFramesV1* s,
        OresLogicalTurnV1 t) { return finish(s, t, 0); }
int ores_logical_frames_v1_cancel(OresNativeActorLogicalFramesV1* s,
        OresLogicalTurnV1 t) { return finish(s, t, 1); }
int ores_logical_frames_v1_close(OresNativeActorLogicalFramesV1* s) {
    if (!lock(s)) return 0;
    for (uint32_t i = 0; i < s->max_turns; ++i)
        if (s->turns[i].active) { unlock(s); return 0; }
    wipe(s->bytes, (size_t)s->max_turns * s->max_depth * s->frame_bytes);
    unlock(s);
    pthread_mutex_destroy(&s->mutex);
    free(s->turns); free(s->frame_generations); free(s->bytes); free(s);
    return 1;
}
