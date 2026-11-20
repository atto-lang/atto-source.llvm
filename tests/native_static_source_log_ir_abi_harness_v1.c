#include "oreslang/runtime/NativeTelemetrySpan.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
extern int64_t hammer(void);
int main(void) {
    OresNativeTelemetryLogEvent events[8];
    ores_native_telemetry_set_enabled(0);
    assert(hammer() == 42);
    assert(ores_native_telemetry_log_stats().pending == 0);
    ores_native_telemetry_set_enabled(1);
    assert(hammer() == 42);
    assert(ores_native_telemetry_log_drain(events, 8) == 2);
    assert(events[0].routine_high == UINT64_C(0xd5f8e146ba97430d));
    assert(events[0].routine_low == UINT64_C(0xac6bf17430477e2a));
    assert(events[0].site_high == UINT64_C(0x652e1632e7464fdb));
    assert(events[0].site_low == UINT64_C(0x936fc40fe24f1c7b));
    assert(events[1].site_high == UINT64_C(0x7777777777777777));
    assert(events[0].native_scope_token != 0);
    assert(events[1].native_scope_token == events[0].native_scope_token);
    assert(strcmp(events[0].message, "hammer.started") == 0);
    assert(strcmp(events[1].message, "hammer.finished") == 0);
    ores_native_telemetry_set_enabled(0);
    assert(hammer() == 42);
    assert(ores_native_telemetry_log_stats().pending == 0);
    ores_native_telemetry_set_enabled(1);
    assert(hammer() == 42);
    assert(ores_native_telemetry_log_drain(events, 8) == 2);
    assert(events[0].native_scope_token != 0);
    ores_native_telemetry_set_enabled(0);
    puts("LLVM_STATIC_SOURCE_LOG_IR_ABI_LINK_HOST_PASS");
    return 0;
}
