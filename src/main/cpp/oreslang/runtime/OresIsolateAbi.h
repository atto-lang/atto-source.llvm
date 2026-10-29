#ifndef ORES_ISOLATE_ABI_H
#define ORES_ISOLATE_ABI_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define ORES_ISOLATE_ABI_VERSION 1u
#define ORES_ISOLATE_I64_SLOTS 16u
typedef struct OresIsolateContext {
    uint32_t abi_version;
    uint32_t slot_count;
    uint64_t generation;
    void* call_data;
    int (*load_i64)(void*, uint32_t, int64_t*);
    int (*store_i64)(void*, uint32_t, int64_t);
} OresIsolateContext;
typedef int64_t (*OresGuestEntry)(OresIsolateContext*, int64_t);
#ifdef __cplusplus
}
#endif
#endif
