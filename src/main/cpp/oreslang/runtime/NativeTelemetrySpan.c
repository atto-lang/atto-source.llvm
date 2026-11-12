#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeTelemetrySpan.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>
#include <time.h>

enum { ORES_TRACE_MAX_DEPTH = 64, ORES_TRACE_MAX_NAME_BYTES = 256 };
typedef struct {
    uint64_t token;
    uint64_t start_ns;
} OresNativeTelemetryFrame;
typedef struct {
    uint64_t next_id;
    uint32_t depth;
    uint64_t started;
    uint64_t ended;
    uint64_t dropped;
    uint64_t last_duration_ns;
    OresNativeTelemetryFrame frames[ORES_TRACE_MAX_DEPTH];
} OresNativeTelemetryCarrier;

static atomic_int ores_telemetry_enabled = ATOMIC_VAR_INIT(0);
static _Thread_local OresNativeTelemetryCarrier ores_telemetry_carrier;

static uint64_t ores_telemetry_monotonic_ns(void) {
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time) != 0) return 0;
    return (uint64_t)time.tv_sec * UINT64_C(1000000000)
            + (uint64_t)time.tv_nsec;
}

void ores_native_telemetry_set_enabled(int enabled) {
    atomic_store_explicit(&ores_telemetry_enabled, enabled ? 1 : 0,
                          memory_order_release);
}

int ores_native_telemetry_is_enabled(void) {
    return atomic_load_explicit(&ores_telemetry_enabled, memory_order_acquire);
}

uint64_t ores_native_telemetry_span_begin(const char* operation, uint64_t length) {
    /* The no-op path must NEVER read the clock or allocate, even temporarily. */
    if (!atomic_load_explicit(&ores_telemetry_enabled, memory_order_relaxed))
        return 0;
    OresNativeTelemetryCarrier* c = &ores_telemetry_carrier;
    if (operation == NULL || length == 0 || length > ORES_TRACE_MAX_NAME_BYTES
            || c->depth >= ORES_TRACE_MAX_DEPTH
            || c->next_id >= (UINT64_MAX >> 7)) {
        ++c->dropped;
        return 0;
    }
    const uint64_t id = ++c->next_id;
    const uint64_t token = (id << 7) | (uint64_t)(c->depth + 1);
    OresNativeTelemetryFrame* frame = &c->frames[c->depth++];
    frame->token = token;
    frame->start_ns = ores_telemetry_monotonic_ns();
    ++c->started;
    return token;
}

void ores_native_telemetry_span_end(uint64_t token) {
    if (token == 0) return;
    OresNativeTelemetryCarrier* c = &ores_telemetry_carrier;
    if (c->depth == 0 || c->frames[c->depth - 1].token != token) {
        /* A stale/cross-carrier/non-LIFO token is never silently accepted. */
        abort();
    }
    OresNativeTelemetryFrame* frame = &c->frames[--c->depth];
    const uint64_t now = ores_telemetry_monotonic_ns();
    c->last_duration_ns = (frame->start_ns != 0 && now >= frame->start_ns)
            ? now - frame->start_ns : 0;
    frame->token = 0;
    frame->start_ns = 0;
    ++c->ended;
}

OresNativeTelemetrySnapshot ores_native_telemetry_snapshot(void) {
    const OresNativeTelemetryCarrier* c = &ores_telemetry_carrier;
    OresNativeTelemetrySnapshot result = {
        c->started, c->ended, c->dropped, (uint64_t)c->depth,
        c->last_duration_ns
    };
    return result;
}
