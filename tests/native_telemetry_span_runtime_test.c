#include "oreslang/runtime/NativeTelemetrySpan.h"
#include <assert.h>
#include <stdint.h>

int main(void) {
    ores_native_telemetry_set_enabled(0);
    assert(ores_native_telemetry_is_enabled() == 0);
    assert(ores_native_telemetry_span_begin("disabled", 8) == 0);
    ores_native_telemetry_span_end(0);
    OresNativeTelemetrySnapshot empty = ores_native_telemetry_snapshot();
    assert(empty.started == 0 && empty.ended == 0 && empty.active == 0);
    ores_native_telemetry_set_enabled(1);
    const uint64_t outer = ores_native_telemetry_span_begin("outer", 5);
    const uint64_t inner = ores_native_telemetry_span_begin("inner", 5);
    assert(outer != 0 && inner != 0 && outer != inner);
    assert(ores_native_telemetry_snapshot().active == 2);
    // A runtime toggle affects new spans only; live spans still must close.
    ores_native_telemetry_set_enabled(0);
    assert(ores_native_telemetry_span_begin("now-off", 7) == 0);
    ores_native_telemetry_span_end(inner);
    ores_native_telemetry_span_end(outer);
    const OresNativeTelemetrySnapshot done = ores_native_telemetry_snapshot();
    assert(done.started == 2 && done.ended == 2 && done.active == 0);
    assert(done.dropped == 0);
    ores_native_telemetry_set_enabled(1);
    // Bounded capacity: the 65th enabled scope returns a safe inert token.
    uint64_t live[64];
    for (unsigned i = 0; i < 64; ++i) {
        live[i] = ores_native_telemetry_span_begin("depth", 5);
        assert(live[i] != 0);
    }
    assert(ores_native_telemetry_span_begin("overflow", 8) == 0);
    assert(ores_native_telemetry_snapshot().dropped == 1);
    for (int i = 63; i >= 0; --i) ores_native_telemetry_span_end(live[i]);
    const OresNativeTelemetrySnapshot finished = ores_native_telemetry_snapshot();
    assert(finished.active == 0 && finished.started == finished.ended);
    assert(finished.started == 66 && finished.dropped == 1);
    return 0;
}
