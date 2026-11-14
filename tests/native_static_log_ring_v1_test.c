#include "oreslang/runtime/NativeTelemetrySpan.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const uint64_t rid_hi = UINT64_C(0xd5f8e146ba97430d);
static const uint64_t rid_lo = UINT64_C(0xac6bf17430477e2a);
static const uint64_t sid_hi = UINT64_C(0x652e1632e7464fdb);
static const uint64_t sid_lo = UINT64_C(0x936fc40fe24f1c7b);

int main(void) {
    OresNativeTelemetryLogEvent events[ORES_NATIVE_TELEMETRY_LOG_CAPACITY];
    ores_native_telemetry_set_enabled(0);
    assert(ores_native_telemetry_log_info(0, 0, 0, 0, NULL, 0) == 0);
    assert(ores_native_telemetry_log_stats().written == 0);

    ores_native_telemetry_set_enabled(1);
    uint64_t scope = ores_native_telemetry_span_begin("hammer", 6);
    assert(scope != 0);
    assert(ores_native_telemetry_log_info(
        rid_hi, rid_lo, sid_hi, sid_lo, "hammer.started", 14) == 1);
    assert(ores_native_telemetry_log_info(
        rid_hi, rid_lo, UINT64_C(0x7777777777777777),
        UINT64_C(0x7777777777777777), "hammer.finished", 15) == 1);
    ores_native_telemetry_span_end(scope);
    OresNativeTelemetryLogStats prior = ores_native_telemetry_log_stats();
    assert(prior.written == 2 && prior.pending == 2 && prior.dropped == 0);
    assert(ores_native_telemetry_log_drain(events, 2) == 2);
    assert(events[0].routine_high == rid_hi && events[0].routine_low == rid_lo);
    assert(events[0].site_high == sid_hi && events[0].site_low == sid_lo);
    assert(events[0].native_scope_token == scope);
    assert(events[1].native_scope_token == scope);
    assert(events[0].message_length == 14);
    assert(strcmp(events[0].message, "hammer.started") == 0);
    assert(strcmp(events[1].message, "hammer.finished") == 0);
    assert(ores_native_telemetry_log_stats().pending == 0);

    for (unsigned i = 0; i < ORES_NATIVE_TELEMETRY_LOG_CAPACITY; ++i) {
        assert(ores_native_telemetry_log_info(
            rid_hi, rid_lo, sid_hi, sid_lo, "event", 5) == 1);
    }
    assert(ores_native_telemetry_log_info(
        rid_hi, rid_lo, sid_hi, sid_lo, "overflow", 8) == 0);
    assert(ores_native_telemetry_log_stats().pending ==
           ORES_NATIVE_TELEMETRY_LOG_CAPACITY);
    assert(ores_native_telemetry_log_drain(
        events, ORES_NATIVE_TELEMETRY_LOG_CAPACITY) ==
        ORES_NATIVE_TELEMETRY_LOG_CAPACITY);
    assert(ores_native_telemetry_log_drain(events, 1) == 0);
    const uint64_t before = ores_native_telemetry_log_stats().dropped;
    assert(ores_native_telemetry_log_info(
        0, 0, sid_hi, sid_lo, "bad", 3) == 0);
    assert(ores_native_telemetry_log_info(
        rid_hi, rid_lo, sid_hi, sid_lo, "a\nb", 3) == 0);
    assert(ores_native_telemetry_log_stats().dropped == before + 2);
    ores_native_telemetry_set_enabled(0);
    assert(ores_native_telemetry_log_info(
        rid_hi, rid_lo, sid_hi, sid_lo, "ignored", 7) == 0);
    assert(ores_native_telemetry_log_stats().pending == 0);

    puts("LLVM_STATIC_LOG_RING_HOST_PASS");
    return 0;
}
