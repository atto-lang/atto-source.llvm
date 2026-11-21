#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#ifdef __cplusplus
extern "C" {
#endif

/* HOST ONLY; experimental, not the compiler ABI, no guest pointer exposure.
 * The scheduler must prove single-turn admission and callback/loan quiescence.
 * Distinct actor arenas can have equal ref32 words without shared authority. */
#define ORES_ELASTIC_NATIVE_MAX_BYTES_V1 (64u * 1024u * 1024u)
#define ORES_ELASTIC_NATIVE_MAX_SLOTS_V1 4096u

typedef struct {
    _Atomic(size_t) charged_bytes;
    size_t limit_bytes;
} OresElasticNativeBudgetV1;

typedef struct OresElasticNativeArenaV1 OresElasticNativeArenaV1;
typedef struct {
    uint64_t domain;
    uint64_t incarnation;
    uint32_t word;
    uint32_t slot;
    uint32_t generation;
} OresElasticNativeHandleV1;

/* Configure process and tenant budgets before exposing them to actor threads.
 * The two pointers can be identical, in which case each charge occurs once. */
int ores_elastic_budget_init_v1(OresElasticNativeBudgetV1* budget, size_t limit_bytes);
size_t ores_elastic_budget_used_v1(const OresElasticNativeBudgetV1* budget);
/* Pure reference-ABI helpers, not capabilities. 0 is null; nonzero word
 * encodes an eight-byte-aligned logical byte offset as offset/8+1.
 * The helpers never authenticate actor authority or expose native addresses. */
int ores_elastic_native_ref32_encode_v1(uint64_t aligned_byte_offset, uint32_t* out_word);
int ores_elastic_native_ref32_decode_v1(uint32_t word, uint64_t* out_byte_offset);
OresElasticNativeArenaV1* ores_elastic_native_open_v1(
    uint64_t actor_id, uint64_t incarnation, size_t hard_limit_bytes,
    uint32_t max_slots, OresElasticNativeBudgetV1* tenant,
    OresElasticNativeBudgetV1* process);
/* Allocation returns 0 on over-quota/OOM; unchanged prior backing and values.
 * Output must be an 8-byte aligned logical ref with out-of-line authority. */
int ores_elastic_native_allocate_v1(OresElasticNativeArenaV1*, size_t bytes,
                                    uint32_t alignment, OresElasticNativeHandleV1* out);
int ores_elastic_native_write_i64_v1(OresElasticNativeArenaV1*,
                                     OresElasticNativeHandleV1, size_t relative_offset,
                                     int64_t value);
int ores_elastic_native_read_i64_v1(OresElasticNativeArenaV1*,
                                    OresElasticNativeHandleV1, size_t relative_offset,
                                    int64_t* out);
int ores_elastic_native_drop_v1(OresElasticNativeArenaV1*, OresElasticNativeHandleV1);
size_t ores_elastic_native_committed_v1(const OresElasticNativeArenaV1*);
size_t ores_elastic_native_live_v1(const OresElasticNativeArenaV1*);
void ores_elastic_native_close_v1(OresElasticNativeArenaV1*);
#ifdef ORES_ELASTIC_TESTING
void ores_elastic_native_fail_next_growth_for_test_v1(OresElasticNativeArenaV1*);
#endif
#ifdef __cplusplus
}
#endif
