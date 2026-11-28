#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeTelemetrySpan.h"

#include <stdatomic.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

enum { ORES_TRACE_MAX_DEPTH = 64, ORES_TRACE_MAX_NAME_BYTES = 256 };
/* 20-bit globally unique carrier identity, 37-bit per-carrier generation,
 * 7-bit stack slot. Exhaustion drops observations, never reuses a token.
 */
#define ORES_TRACE_MAX_CARRIER_ID ((UINT64_C(1) << 20) - 1)
#define ORES_TRACE_MAX_GENERATION ((UINT64_C(1) << 37) - 1)
typedef struct {
    uint64_t token;
    uint64_t start_ns;
} OresNativeTelemetryFrame;
typedef struct {
    uint64_t carrier_id;
    uint64_t next_id;
    uint32_t depth;
    uint64_t started;
    uint64_t ended;
    uint64_t dropped;
    uint64_t last_duration_ns;
    OresNativeTelemetryFrame frames[ORES_TRACE_MAX_DEPTH];
    uint64_t log_written;
    uint64_t log_consumed;
    uint64_t log_dropped;
    OresNativeTelemetryLogEvent log_events[ORES_NATIVE_TELEMETRY_LOG_CAPACITY];
} OresNativeTelemetryCarrier;

static atomic_int ores_telemetry_enabled = 0;
static atomic_uint_fast64_t ores_telemetry_next_carrier = 1;
static _Thread_local OresNativeTelemetryCarrier ores_telemetry_carrier;

/* Each carrier receives a unique, never-reused ID before its first span.
 * No allocation, OS thread-ID reuse, or host-pointer identity is involved.
 * The compare/exchange saturates instead of wrapping to a previously used ID.
 */
static uint64_t ores_telemetry_carrier_id(OresNativeTelemetryCarrier* c) {
    if (c->carrier_id != 0) return c->carrier_id;
    uint_fast64_t next = atomic_load_explicit(
            &ores_telemetry_next_carrier, memory_order_relaxed);
    while (next <= ORES_TRACE_MAX_CARRIER_ID) {
        if (atomic_compare_exchange_weak_explicit(
                &ores_telemetry_next_carrier, &next, next + 1,
                memory_order_relaxed, memory_order_relaxed)) {
            c->carrier_id = (uint64_t)next;
            return c->carrier_id;
        }
    }
    return 0;
}

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
            || c->next_id >= ORES_TRACE_MAX_GENERATION
            || ores_telemetry_carrier_id(c) == 0) {
        ++c->dropped;
        return 0;
    }
    const uint64_t id = ++c->next_id;
    const uint64_t token = (c->carrier_id << 44) | (id << 7)
            | (uint64_t)(c->depth + 1);
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

/* Every receipt is copied into a carrier-local fixed array. No heap,
 * formatter, output sink or waiting when guest actor code publishes. */
static void ores_native_telemetry_log_drop(OresNativeTelemetryCarrier* c) {
    if (c->log_dropped != UINT64_MAX) ++c->log_dropped;
}

int ores_native_telemetry_log_info(uint64_t routine_high, uint64_t routine_low,
                                  uint64_t site_high, uint64_t site_low,
                                  const char* message, uint64_t length) {
    if (!atomic_load_explicit(&ores_telemetry_enabled, memory_order_relaxed))
        return 0;
    OresNativeTelemetryCarrier* c = &ores_telemetry_carrier;
    if ((routine_high | routine_low) == 0 ||
        (site_high | site_low) == 0 ||
        message == NULL || length == 0 ||
        length > ORES_NATIVE_TELEMETRY_LOG_MESSAGE_BYTES ||
        c->log_written == UINT64_MAX) {
        ores_native_telemetry_log_drop(c);
        return 0;
    }
    for (uint64_t i = 0; i < length; ++i) {
        const unsigned char byte = (unsigned char)message[i];
        if (byte < 32 || byte == 127) {
            ores_native_telemetry_log_drop(c);
            return 0;
        }
    }
    if (c->log_written - c->log_consumed >=
        ORES_NATIVE_TELEMETRY_LOG_CAPACITY) {
        ores_native_telemetry_log_drop(c);
        return 0;
    }
    OresNativeTelemetryLogEvent* event =
        &c->log_events[c->log_written % ORES_NATIVE_TELEMETRY_LOG_CAPACITY];
    event->routine_high = routine_high;
    event->routine_low = routine_low;
    event->site_high = site_high;
    event->site_low = site_low;
    event->native_scope_token =
        c->depth > 0 ? c->frames[c->depth - 1].token : 0;
    event->message_length = (uint32_t)length;
    memcpy(event->message, message, (size_t)length);
    event->message[length] = '\0';
    ++c->log_written;
    return 1;
}

uint64_t ores_native_telemetry_log_drain(
    OresNativeTelemetryLogEvent* destination, uint64_t max_events) {
    if (destination == NULL || max_events == 0) return 0;
    OresNativeTelemetryCarrier* c = &ores_telemetry_carrier;
    uint64_t taken = 0;
    while (taken < max_events && c->log_consumed < c->log_written) {
        OresNativeTelemetryLogEvent* next =
            &c->log_events[c->log_consumed % ORES_NATIVE_TELEMETRY_LOG_CAPACITY];
        destination[taken++] = *next;
        memset(next, 0, sizeof(*next)); /* erase stale literal and IDs */
        ++c->log_consumed;
    }
    return taken;
}

OresNativeTelemetryLogStats ores_native_telemetry_log_stats(void) {
    const OresNativeTelemetryCarrier* c = &ores_telemetry_carrier;
    OresNativeTelemetryLogStats stats = {
        c->log_written, c->log_dropped,
        c->log_written - c->log_consumed
    };
    return stats;
}
