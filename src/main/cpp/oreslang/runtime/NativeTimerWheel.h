#ifndef ORESLANG_RUNTIME_NATIVE_TIMER_WHEEL_H
#define ORESLANG_RUNTIME_NATIVE_TIMER_WHEEL_H

/*
 * Allocation-free hierarchical timing wheel: the native runtime's logical
 * timer engine (contracts/v1/TIMER_ENGINE_V1.md). Same algorithm and emission
 * order as the Java backend's OresTimerWheel; independent code, shared
 * conformance vector.
 *
 * Six levels of 64 slots cover 2^36 ticks; later deadlines wait on an overflow
 * list. A timer is placed by the highest 6-bit tick group in which its expiry
 * tick differs from the current tick. Arm/cancel are O(1); advancing jumps
 * between occupied slots via per-level occupancy bitmaps.
 *
 * Records live in one slab allocated at init (capacity fixed thereafter).
 * Handles are (generation << 32) | (index + 1); 0 is never valid. A slot's
 * generation advances when the record is freed; exhausted slots retire instead
 * of recycling a stale handle.
 *
 * The caller supplies monotonic nanoseconds on every call (virtual clocks are
 * fine). A timer fires on the first poll at or past its deadline, never
 * earlier. Expired timers are emitted in expiry-tick order and, within a tick,
 * in the order they reached it; at most `max` per poll; queued timers remain
 * cancellable until emitted.
 *
 * Not thread-safe: one owner serialises all calls. No call allocates after
 * ores_timer_wheel_init, and no call ever invokes guest code.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ORES_TIMER_WHEEL_LEVELS 6
#define ORES_TIMER_WHEEL_SLOTS 64
#define ORES_TIMER_WHEEL_LISTS (ORES_TIMER_WHEEL_LEVELS * ORES_TIMER_WHEEL_SLOTS + 3)
#define ORES_TIMER_NO_DEADLINE UINT64_MAX

typedef struct OresTimerExpired {
    uint64_t handle;  /* already invalid when returned */
    uint64_t payload;
    uint64_t deadline_ns;
} OresTimerExpired;

typedef struct OresTimerWheel {
    uint64_t tick_ns;
    uint64_t current_tick;
    uint64_t now_ns;
    uint32_t capacity;
    uint32_t size;
    int32_t free_head;
    int32_t event_level;
    uint64_t* deadline;
    uint64_t* payload;
    int32_t* next;
    int32_t* prev;
    int32_t* where;
    uint32_t* generation;
    int32_t head[ORES_TIMER_WHEEL_LISTS];
    int32_t tail[ORES_TIMER_WHEEL_LISTS];
    uint64_t occupancy[ORES_TIMER_WHEEL_LEVELS];
} OresTimerWheel;

/* Allocates the slab once. Returns false on bad arguments or OOM. */
bool ores_timer_wheel_init(OresTimerWheel* w, uint32_t capacity,
                           uint64_t tick_ns, uint64_t now_ns);
void ores_timer_wheel_destroy(OresTimerWheel* w);

/* Absolute monotonic deadline. Returns 0 when no reusable slot remains or
 * the reserved ORES_TIMER_NO_DEADLINE sentinel is supplied. */
uint64_t ores_timer_wheel_arm(OresTimerWheel* w, uint64_t deadline_ns, uint64_t payload);
/* True only if the handle named an armed or queued timer. */
bool ores_timer_wheel_cancel(OresTimerWheel* w, uint64_t handle);
bool ores_timer_wheel_is_live(const OresTimerWheel* w, uint64_t handle);
/* Advance (clamped monotonic) and emit up to max expired timers into out. */
size_t ores_timer_wheel_poll(OresTimerWheel* w, uint64_t now_ns,
                             OresTimerExpired* out, size_t max);
/* now_ns if timers are due, else a lower bound on the next deadline, or
 * ORES_TIMER_NO_DEADLINE. Never later than the true next deadline. */
uint64_t ores_timer_wheel_next_deadline_ns(OresTimerWheel* w);

#ifdef __cplusplus
}
#endif

#endif
