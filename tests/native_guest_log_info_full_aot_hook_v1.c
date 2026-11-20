// Linked-guest proof hook for the actual LLVM-compiled .atto main() fixture.
// There is intentionally NO C main(): the guest compiler must provide it.
// The process must exit 42 and the same main carrier must retain both sites.
#include "oreslang/runtime/NativeTelemetrySpan.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(__GNUC__) && !defined(__clang__)
#error "linked Atto guest lifecycle proof currently requires Clang/GNU constructors"
#endif

__attribute__((constructor))
static void before_atto_guest_main(void) {
    ores_native_telemetry_set_enabled(1);
}

__attribute__((destructor))
static void after_atto_guest_main(void) {
    OresNativeTelemetryLogEvent events[4];
    if (ores_native_telemetry_log_drain(events, 4) != 2 ||
        events[0].routine_high != UINT64_C(0xd5f8e146ba97430d) ||
        events[0].routine_low != UINT64_C(0xac6bf17430477e2a) ||
        events[0].site_high != UINT64_C(0x652e1632e7464fdb) ||
        events[0].site_low != UINT64_C(0x936fc40fe24f1c7b) ||
        events[1].site_high != UINT64_C(0x7777777777777777) ||
        events[1].site_low != UINT64_C(0x7777777777777777) ||
        events[0].native_scope_token == 0 ||
        events[1].native_scope_token != events[0].native_scope_token ||
        events[0].message_length != 14 ||
        events[1].message_length != 15 ||
        strcmp(events[0].message, "hammer.started") != 0 ||
        strcmp(events[1].message, "hammer.finished") != 0 ||
        ores_native_telemetry_log_stats().pending != 0) {
        fputs("LLVM_NATIVE_GENERATED_GUEST_LOG_RECEIPT_MISMATCH\n", stderr);
        _Exit(98);
    }
    puts("LLVM_NATIVE_LOG_INFO_GENERATED_GUEST_PASS");
    fflush(stdout);
}
