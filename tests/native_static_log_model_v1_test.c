#include "oreslang/runtime/NativeTelemetrySpan.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define STEPS 50000u
static uint64_t rng_state = UINT64_C(0x123456789fedcba9);
static uint64_t draw(void) {
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return rng_state * UINT64_C(2685821657736338717);
}

typedef struct {
    uint64_t rhi, rlo, shi, slo;
    uint32_t length;
    char message[ORES_NATIVE_TELEMETRY_LOG_MESSAGE_BYTES + 1];
} Expected;

int main(void) {
    Expected ring[ORES_NATIVE_TELEMETRY_LOG_CAPACITY] = {0};
    uint64_t head = 0, count = 0, written = 0, dropped = 0;
    OresNativeTelemetryLogEvent actual[ORES_NATIVE_TELEMETRY_LOG_CAPACITY];
    char message[ORES_NATIVE_TELEMETRY_LOG_MESSAGE_BYTES + 32];
    int enabled = 0;
    ores_native_telemetry_set_enabled(0);

    for (unsigned step = 0; step < STEPS; ++step) {
        unsigned action = (unsigned)(draw() % 11);
        if (action == 0) {
            enabled = !enabled;
            ores_native_telemetry_set_enabled(enabled);
        } else if (action == 1 || action == 2) {
            const uint64_t request = draw() % (ORES_NATIVE_TELEMETRY_LOG_CAPACITY + 9u);
            const uint64_t take = request < count ? request : count;
            const uint64_t received = ores_native_telemetry_log_drain(actual, request);
            assert(received == take);
            for (uint64_t i = 0; i < received; ++i) {
                const Expected* expected = &ring[(head + i) % ORES_NATIVE_TELEMETRY_LOG_CAPACITY];
                assert(actual[i].routine_high == expected->rhi);
                assert(actual[i].routine_low == expected->rlo);
                assert(actual[i].site_high == expected->shi);
                assert(actual[i].site_low == expected->slo);
                assert(actual[i].message_length == expected->length);
                assert(memcmp(actual[i].message, expected->message, expected->length + 1) == 0);
                assert(actual[i].native_scope_token == 0);
            }
            head += received;
            count -= received;
        } else {
            uint64_t rhi = (draw() % 9 == 0) ? 0 : UINT64_C(0x1111);
            uint64_t rlo = (draw() % 9 == 0) ? 0 : UINT64_C(0x2222);
            uint64_t shi = (draw() % 9 == 0) ? 0 : UINT64_C(0x3333);
            uint64_t slo = (draw() % 9 == 0) ? 0 : UINT64_C(0x4444);
            uint64_t len = draw() % sizeof(message);
            int valid = len > 0 && len <= ORES_NATIVE_TELEMETRY_LOG_MESSAGE_BYTES;
            for (uint64_t i = 0; i < len; ++i) {
                message[i] = draw() % 19 == 0 ? '\n' : (char)('A' + (draw() % 26));
                if ((unsigned char)message[i] < 32) valid = 0;
            }
            if ((rhi | rlo) == 0 || (shi | slo) == 0) valid = 0;
            int success = enabled && valid && count < ORES_NATIVE_TELEMETRY_LOG_CAPACITY;
            int actual_return = ores_native_telemetry_log_info(
                rhi, rlo, shi, slo, message, len);
            assert(actual_return == success);
            if (success) {
                Expected* e = &ring[(head + count) % ORES_NATIVE_TELEMETRY_LOG_CAPACITY];
                e->rhi = rhi; e->rlo = rlo; e->shi = shi; e->slo = slo;
                e->length = (uint32_t)len;
                memcpy(e->message, message, (size_t)len);
                e->message[len] = 0;
                ++written; ++count;
            } else if (enabled) {
                ++dropped;
            }
        }
        OresNativeTelemetryLogStats snapshot = ores_native_telemetry_log_stats();
        assert(snapshot.written == written);
        assert(snapshot.dropped == dropped);
        assert(snapshot.pending == count);
    }
    ores_native_telemetry_set_enabled(0);
    assert(ores_native_telemetry_log_drain(actual, 0) == 0);
    assert(ores_native_telemetry_log_drain(NULL, 8) == 0);
    puts("LLVM_STATIC_LOG_MODEL_HOST_PASS:50000");
    return 0;
}
