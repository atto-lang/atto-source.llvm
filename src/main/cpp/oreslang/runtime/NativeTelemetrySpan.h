#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * LLVM full-AOT dormant-probe ABI v1. These functions are HOST-RUNTIME
 * authority, not names that the guest can call directly.
 *
 * Disabled begin returns token 0 without allocation or clock sampling.
 * Enabled scopes are bounded to 64 per native carrier and strictly LIFO;
 * suspension/carrier migration is not yet admitted by the native compiler.
 * Token 0 is always an inert span whose end is a no-op.
 */
typedef struct OresNativeTelemetrySnapshot {
    uint64_t started;
    uint64_t ended;
    uint64_t dropped;
    uint64_t active;
    uint64_t last_duration_ns;
} OresNativeTelemetrySnapshot;

/* Trusted host/control-plane only. Affects new scopes; existing scopes close. */
void ores_native_telemetry_set_enabled(int enabled);
int ores_native_telemetry_is_enabled(void);

/* Compiler-owned AOT probes; token must never become a guest scalar/handle. */
uint64_t ores_native_telemetry_span_begin(const char* operation, uint64_t length);
void ores_native_telemetry_span_end(uint64_t token);

/* Caller-owned, thread-local snapshot; no pointers or active frames returned. */
OresNativeTelemetrySnapshot ores_native_telemetry_snapshot(void);

#ifdef __cplusplus
}
#endif
