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

/* First native static guest log site: only compiler-checked literal messages
 * (1..96 bytes) and four fixed-width 64-bit ID lanes are admitted.
 * No dynamic formatter, guest object/pointer, clock, OS write or network work.
 * This experimental hosted-AOT lane uses the current native-carrier scoped
 * token; it is NOT W3C trace/span identity or actor migration support. */
#define ORES_NATIVE_TELEMETRY_LOG_CAPACITY 32u
#define ORES_NATIVE_TELEMETRY_LOG_MESSAGE_BYTES 96u
typedef struct OresNativeTelemetryLogEvent {
    uint64_t routine_high, routine_low, site_high, site_low;
    uint64_t native_scope_token;
    uint32_t message_length;
    char message[ORES_NATIVE_TELEMETRY_LOG_MESSAGE_BYTES + 1u];
} OresNativeTelemetryLogEvent;
typedef struct OresNativeTelemetryLogStats {
    uint64_t written, dropped, pending;
} OresNativeTelemetryLogStats;

/* Compiler ABI only; guest source cannot call these C functions directly. */
int ores_native_telemetry_log_info(uint64_t routine_high, uint64_t routine_low,
                                  uint64_t site_high, uint64_t site_low,
                                  const char* literal_message, uint64_t byte_length);
/* Drain only from the same native carrier; no shared mutable ring or lock. */
uint64_t ores_native_telemetry_log_drain(
    OresNativeTelemetryLogEvent* destination, uint64_t max_events);
OresNativeTelemetryLogStats ores_native_telemetry_log_stats(void);

/* Caller-owned, thread-local snapshot; no pointers or active frames returned. */
OresNativeTelemetrySnapshot ores_native_telemetry_snapshot(void);

#ifdef __cplusplus
}
#endif
