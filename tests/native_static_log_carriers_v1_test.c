#include "oreslang/runtime/NativeTelemetrySpan.h"
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

enum { CARRIERS = 8, EVENTS_PER_CARRIER = 1500 };
typedef struct CarrierProof {
    uint64_t span_token;
    uint64_t written;
    uint64_t dropped;
    uint64_t received;
} CarrierProof;

static void* run_carrier(void* input) {
    CarrierProof* proof = (CarrierProof*)input;
    uint64_t token = ores_native_telemetry_span_begin("carrier", 7);
    assert(token != 0);
    for (int i = 0; i < EVENTS_PER_CARRIER; ++i) {
        (void)ores_native_telemetry_log_info(
            UINT64_C(0xd5f8e146ba97430d), UINT64_C(0xac6bf17430477e2a),
            UINT64_C(0x652e1632e7464fdb), UINT64_C(0x936fc40fe24f1c7b),
            "carrier.event", 13);
    }
    OresNativeTelemetryLogStats stats = ores_native_telemetry_log_stats();
    proof->span_token = token;
    proof->written = stats.written;
    proof->dropped = stats.dropped;
    OresNativeTelemetryLogEvent events[ORES_NATIVE_TELEMETRY_LOG_CAPACITY];
    proof->received = ores_native_telemetry_log_drain(
        events, ORES_NATIVE_TELEMETRY_LOG_CAPACITY);
    for (uint64_t i = 0; i < proof->received; ++i) {
        assert(events[i].native_scope_token == token);
        assert(events[i].site_low == UINT64_C(0x936fc40fe24f1c7b));
    }
    assert(ores_native_telemetry_log_stats().pending == 0);
    ores_native_telemetry_span_end(token);
    return NULL;
}

int main(void) {
    pthread_t threads[CARRIERS];
    CarrierProof proofs[CARRIERS] = {0};
    ores_native_telemetry_set_enabled(1);
    for (int i = 0; i < CARRIERS; ++i)
        assert(pthread_create(&threads[i], NULL, run_carrier, &proofs[i]) == 0);
    for (int i = 0; i < CARRIERS; ++i) {
        assert(pthread_join(threads[i], NULL) == 0);
        assert(proofs[i].span_token != 0);
        assert(proofs[i].written == ORES_NATIVE_TELEMETRY_LOG_CAPACITY);
        assert(proofs[i].dropped ==
               EVENTS_PER_CARRIER - ORES_NATIVE_TELEMETRY_LOG_CAPACITY);
        assert(proofs[i].received == ORES_NATIVE_TELEMETRY_LOG_CAPACITY);
        for (int j = 0; j < i; ++j)
            assert(proofs[i].span_token != proofs[j].span_token);
    }
    assert(ores_native_telemetry_log_stats().pending == 0);
    assert(ores_native_telemetry_log_stats().written == 0);
    ores_native_telemetry_set_enabled(0);
    puts("LLVM_STATIC_LOG_RING_CARRIERS_HOST_PASS:8x1500");
    return 0;
}
