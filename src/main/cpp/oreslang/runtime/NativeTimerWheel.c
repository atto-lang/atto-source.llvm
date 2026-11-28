#include "oreslang/runtime/NativeTimerWheel.h"

#include <stdlib.h>
#include <string.h>

#define SLOT_BITS 6
#define SLOTS ORES_TIMER_WHEEL_SLOTS
#define LEVELS ORES_TIMER_WHEEL_LEVELS
#define SPAN_BITS (LEVELS * SLOT_BITS) /* 36 */
#define NIL (-1)
#define FREE (-1)
#define DUE (LEVELS * SLOTS)
#define CURRENT (DUE + 1)
#define OVERFLOW (DUE + 2)
#define NONE UINT64_MAX

static uint64_t make_handle(const OresTimerWheel* w, int32_t i) {
    return ((uint64_t)w->generation[i] << 32) | (uint64_t)(i + 1);
}

static int32_t index_of(const OresTimerWheel* w, uint64_t handle) {
    uint64_t low = handle & 0xffffffffu;
    if (low == 0 || low > w->capacity) return -1;
    int32_t i = (int32_t)(low - 1);
    if (w->generation[i] != (uint32_t)(handle >> 32) || w->where[i] == FREE) return -1;
    return i;
}

static void append(OresTimerWheel* w, int32_t i, int32_t list) {
    w->where[i] = list;
    w->next[i] = NIL;
    w->prev[i] = w->tail[list];
    if (w->tail[list] != NIL) w->next[w->tail[list]] = i; else w->head[list] = i;
    w->tail[list] = i;
}

static void unlink_rec(OresTimerWheel* w, int32_t i) {
    int32_t list = w->where[i], p = w->prev[i], n = w->next[i];
    if (p != NIL) w->next[p] = n; else w->head[list] = n;
    if (n != NIL) w->prev[n] = p; else w->tail[list] = p;
    if (w->head[list] == NIL && list < DUE)
        w->occupancy[list / SLOTS] &= ~(UINT64_C(1) << (list % SLOTS));
    w->next[i] = w->prev[i] = NIL;
}

static int32_t take(OresTimerWheel* w, int32_t list) {
    int32_t first = w->head[list];
    w->head[list] = w->tail[list] = NIL;
    if (list < DUE) w->occupancy[list / SLOTS] &= ~(UINT64_C(1) << (list % SLOTS));
    return first;
}

static void release(OresTimerWheel* w, int32_t i) {
    w->payload[i] = 0;
    w->where[i] = FREE;
    uint32_t g = w->generation[i] + 1;
    w->generation[i] = g == 0 ? 1 : g;
    w->next[i] = w->free_head;
    w->free_head = i;
    w->size--;
}

/* Cascades must not shortcut to DUE using the final poll time. */
static void place_by_tick(OresTimerWheel* w, int32_t i) {
    uint64_t e = w->deadline[i] / w->tick_ns;
    if (e <= w->current_tick) { append(w, i, CURRENT); return; }
    int level = (63 - __builtin_clzll(e ^ w->current_tick)) / SLOT_BITS;
    if (level >= LEVELS) { append(w, i, OVERFLOW); return; }
    int s = (int)((e >> (level * SLOT_BITS)) & (SLOTS - 1));
    append(w, i, level * SLOTS + s);
    w->occupancy[level] |= UINT64_C(1) << s;
}

static void place(OresTimerWheel* w, int32_t i) {
    if (w->deadline[i] <= w->now_ns) append(w, i, DUE);
    else place_by_tick(w, i);
}

static uint64_t next_event_tick(OresTimerWheel* w) {
    for (int level = 0; level < LEVELS; level++) {
        uint64_t occ = w->occupancy[level];
        if (occ == 0) continue;
        int shift = level * SLOT_BITS;
        int cur = (int)((w->current_tick >> shift) & (SLOTS - 1));
        uint64_t above = cur == SLOTS - 1 ? 0 : occ & (~UINT64_C(0) << (cur + 1));
        if (above == 0) continue;
        int s = __builtin_ctzll(above);
        w->event_level = level;
        return ((w->current_tick >> (shift + SLOT_BITS)) << (shift + SLOT_BITS))
               | ((uint64_t)s << shift);
    }
    if (w->head[OVERFLOW] != NIL) {
        w->event_level = -1;
        return ((w->current_tick >> SPAN_BITS) + 1) << SPAN_BITS;
    }
    return NONE;
}

static void drain_current(OresTimerWheel* w, bool all) {
    int32_t i = w->head[CURRENT];
    while (i != NIL) {
        int32_t n = w->next[i];
        if (all || w->deadline[i] <= w->now_ns) {
            unlink_rec(w, i);
            append(w, i, DUE);
        }
        i = n;
    }
}

static void advance(OresTimerWheel* w, uint64_t now) {
    if (now < w->now_ns) now = w->now_ns;
    uint64_t target = now / w->tick_ns;
    w->now_ns = now;
    for (;;) {
        uint64_t t = next_event_tick(w);
        if (t == NONE || t > target) break;
        drain_current(w, true);
        w->current_tick = t;
        int level = w->event_level;
        int32_t list = level < 0 ? OVERFLOW
            : level * SLOTS + (int32_t)((t >> (level * SLOT_BITS)) & (SLOTS - 1));
        int32_t first = take(w, list);
        while (first != NIL) {
            int32_t i = first;
            first = w->next[i];
            place_by_tick(w, i);
        }
    }
    if (target > w->current_tick) {
        drain_current(w, true);
        w->current_tick = target;
    }
    drain_current(w, false);
}

bool ores_timer_wheel_init(OresTimerWheel* w, uint32_t capacity,
                           uint64_t tick_ns, uint64_t now_ns) {
    if (!w || capacity == 0 || capacity > INT32_MAX || tick_ns == 0) return false;
    memset(w, 0, sizeof(*w));
    w->tick_ns = tick_ns;
    w->now_ns = now_ns;
    w->current_tick = now_ns / tick_ns;
    w->capacity = capacity;
    w->deadline = calloc(capacity, sizeof(uint64_t));
    w->payload = calloc(capacity, sizeof(uint64_t));
    w->next = calloc(capacity, sizeof(int32_t));
    w->prev = calloc(capacity, sizeof(int32_t));
    w->where = calloc(capacity, sizeof(int32_t));
    w->generation = calloc(capacity, sizeof(uint32_t));
    if (!w->deadline || !w->payload || !w->next || !w->prev || !w->where || !w->generation) {
        ores_timer_wheel_destroy(w);
        return false;
    }
    for (int k = 0; k < ORES_TIMER_WHEEL_LISTS; k++) w->head[k] = w->tail[k] = NIL;
    w->free_head = NIL;
    for (int64_t i = (int64_t)capacity - 1; i >= 0; i--) {
        w->generation[i] = 1;
        w->where[i] = FREE;
        w->next[i] = w->free_head;
        w->free_head = (int32_t)i;
    }
    return true;
}

void ores_timer_wheel_destroy(OresTimerWheel* w) {
    if (!w) return;
    free(w->deadline); free(w->payload); free(w->next);
    free(w->prev); free(w->where); free(w->generation);
    w->deadline = w->payload = NULL;
    w->next = w->prev = w->where = NULL;
    w->generation = NULL;
    w->capacity = w->size = 0;
    w->free_head = NIL;
}

uint64_t ores_timer_wheel_arm(OresTimerWheel* w, uint64_t deadline_ns, uint64_t payload) {
    if (w->free_head == NIL) return 0;
    int32_t i = w->free_head;
    w->free_head = w->next[i];
    w->deadline[i] = deadline_ns;
    w->payload[i] = payload;
    w->size++;
    place(w, i);
    return make_handle(w, i);
}

bool ores_timer_wheel_cancel(OresTimerWheel* w, uint64_t handle) {
    int32_t i = index_of(w, handle);
    if (i < 0) return false;
    unlink_rec(w, i);
    release(w, i);
    return true;
}

bool ores_timer_wheel_is_live(const OresTimerWheel* w, uint64_t handle) {
    return index_of(w, handle) >= 0;
}

size_t ores_timer_wheel_poll(OresTimerWheel* w, uint64_t now_ns,
                             OresTimerExpired* out, size_t max) {
    advance(w, now_ns);
    size_t n = 0;
    while (n < max && w->head[DUE] != NIL) {
        int32_t i = w->head[DUE];
        unlink_rec(w, i);
        out[n].handle = make_handle(w, i);
        out[n].payload = w->payload[i];
        out[n].deadline_ns = w->deadline[i];
        release(w, i);
        n++;
    }
    return n;
}

uint64_t ores_timer_wheel_next_deadline_ns(OresTimerWheel* w) {
    if (w->head[DUE] != NIL) return w->now_ns;
    uint64_t best = NONE;
    for (int32_t i = w->head[CURRENT]; i != NIL; i = w->next[i])
        if (w->deadline[i] < best) best = w->deadline[i];
    if (best != NONE) return best;
    uint64_t t = next_event_tick(w);
    if (t == NONE) return ORES_TIMER_NO_DEADLINE;
    return t > (UINT64_MAX - 1) / w->tick_ns ? ORES_TIMER_NO_DEADLINE - 1 : t * w->tick_ns;
}
