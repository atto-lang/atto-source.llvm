#ifndef ORES_ISOLATE_ABI_H
#define ORES_ISOLATE_ABI_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define ORES_ISOLATE_ABI_VERSION 2u
#define ORES_ISOLATE_I64_SLOTS 16u
typedef struct OresIsolateContext {
    uint32_t abi_version;
    uint32_t slot_count;
    uint64_t generation;
    void* call_data;
    int (*load_i64)(void*, uint32_t, int64_t*);
    int (*store_i64)(void*, uint32_t, int64_t);
} OresIsolateContext;
// Version 2: status and signed result occupy independent ABI channels.
// out_value is meaningful ONLY when status == ORES_GUEST_OK.
// Callers must not confuse valid negative i64 results with failure codes.
#define ORES_GUEST_OK 0
#define ORES_GUEST_BAD_CONTEXT 1
#define ORES_GUEST_LOAD_DENIED 2
#define ORES_GUEST_STORE_DENIED 3
#define ORES_GUEST_OVERFLOW 4
#define ORES_GUEST_FAILED 5
typedef int32_t (*OresGuestEntry)(OresIsolateContext*, int64_t, int64_t* out_value);

#ifdef __cplusplus
}
#endif
#endif
