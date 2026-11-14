/* Oreslang Universal Hot Load host adapter ABI v1.
 * Proposed interface only: no backend is certified by this header.
 * IN-PROCESS TRUSTED HOST CONTROL ONLY. Not a network, Wasm guest or Oreslang
 * source ABI; never pass untrusted guest pointers, capabilities or source data.
 */
#ifndef ORES_HOT_LOAD_V1_H
#define ORES_HOT_LOAD_V1_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define ORES_HOT_LOAD_ABI_VERSION 1u
/* Stable host-native dynamic symbol, never imported into untrusted guest code. */
#define ORES_HOT_LOAD_ENTRY_SYMBOL "ores_hot_query_api_v1"
#define ORES_HOT_FEAT_CODE_ONLY           (UINT64_C(1) << 0)
#define ORES_HOT_FEAT_STATE_MIGRATION     (UINT64_C(1) << 1)
#define ORES_HOT_FEAT_SUPERVISED_SUBTREE  (UINT64_C(1) << 2)
#define ORES_HOT_FEAT_ETS_HEIR            (UINT64_C(1) << 3)
#define ORES_HOT_FEAT_ETS_MIGRATION       (UINT64_C(1) << 4)
#define ORES_HOT_FEAT_UNTRUSTED           (UINT64_C(1) << 5)
#define ORES_HOT_FEAT_NATIVE_UNLOAD       (UINT64_C(1) << 6)
#define ORES_HOT_MODE_AOT_TO_AOT          (UINT64_C(1) << 16)
#define ORES_HOT_MODE_AOT_TO_JIT          (UINT64_C(1) << 17)
#define ORES_HOT_MODE_JIT_TO_AOT          (UINT64_C(1) << 18)
#define ORES_HOT_MODE_JIT_TO_JIT          (UINT64_C(1) << 19)

typedef enum ores_hot_status_v1 {
    ORES_HOT_OK = 0,
    ORES_HOT_UNSUPPORTED = 1,
    ORES_HOT_DENIED = 2,
    ORES_HOT_BAD_ABI = 3,
    ORES_HOT_STALE_EPOCH = 4,
    ORES_HOT_BUSY = 5,
    ORES_HOT_RESOURCE_LIMIT = 6,
    ORES_HOT_MIGRATION_FAILED = 7,
    ORES_HOT_QUARANTINED = 8,
    ORES_HOT_BAD_HANDLE = 9,
    ORES_HOT_INTERNAL = 10
} ores_hot_status_v1;

/* These are opaque registry identifiers, NEVER authority by themselves.
 * Host adapters must reject fabricated, stale, foreign and cross-tenant IDs.
 * No actor heap, guest closure, code-image or FFI pointer may cross here.
 */
typedef struct ores_hot_id_v1 {
    uint64_t high;
    uint64_t low;
} ores_hot_id_v1;

/* The byte span is call-scoped, read-only, length-bounded, and host-owned.
 * Receivers copy before asynchronous processing; no pointer retained.
 * Manifests/checkpoints have independently specified canonical formats.
 */
typedef struct ores_hot_bytes_v1 {
    const uint8_t *data;
    uint64_t length;
} ores_hot_bytes_v1;

typedef struct ores_hot_capabilities_v1 {
    uint32_t struct_size;             /* sizeof(ores_hot_capabilities_v1) */
    uint32_t abi_version;             /* exactly 1 */
    uint64_t features;                /* ORES_HOT_FEAT_* and MODE_* */
    uint64_t max_manifest_bytes;
    uint64_t max_checkpoint_bytes;
    uint64_t max_live_generations;
} ores_hot_capabilities_v1;

/* All calls must be mediated by a trusted-runtime issuer bound to tenant,
 * supervisor, actor subtree, generation and lifetime. No function may invoke
 * guest code while a supervisor/loader publication lock is held.
 * All status outputs are defined only when status == ORES_HOT_OK.
 *
 * stage: verified manifest + immutable image reference; never a raw path.
 * prepare: opaque bounded release plan incl child specs, ETS and state rules.
 * freeze: DATA admission stops; fatal CONTROL/cancel/replies continue.
 * quiesce: NONBLOCKING poll; BUSY means not yet drained, never spin in carrier.
 * checkpoint: writes into caller-owned bounded buffer, reports exact byte count.
 * migrate: validates independently owned staging state/table snapshots.
 * commit: one CAS publication of route+image+state+child specs+ETS epoch.
 * abort: may discard ONLY precommit transaction; idempotent cleanup.
 * ack_ets_handoff: single-use heir receipt after fenced commit.
 * retire: BUSY while any image/frame/task/FFI/table reader pin exists.
 */
typedef struct ores_hot_api_v1 {
    uint32_t struct_size;   /* input/output structure-size negotiation */
    uint32_t abi_version;   /* exactly 1 */
    void *trusted_runtime_context; /* host only; no guest access */
    ores_hot_status_v1 (*query_capabilities)(
        void *ctx, ores_hot_capabilities_v1 *out);
    ores_hot_status_v1 (*stage)(
        void *ctx, ores_hot_id_v1 supervisor_authority,
        ores_hot_bytes_v1 authenticated_manifest,
        ores_hot_id_v1 *out_transaction);
    ores_hot_status_v1 (*prepare)(
        void *ctx, ores_hot_id_v1 transaction, ores_hot_bytes_v1 release_plan);
    ores_hot_status_v1 (*freeze)(
        void *ctx, ores_hot_id_v1 transaction, uint64_t expected_route_epoch);
    ores_hot_status_v1 (*quiesce)(
        void *ctx, ores_hot_id_v1 transaction);
    ores_hot_status_v1 (*checkpoint)(
        void *ctx, ores_hot_id_v1 transaction, uint8_t *buffer,
        uint64_t capacity, uint64_t *out_length);
    ores_hot_status_v1 (*migrate)(
        void *ctx, ores_hot_id_v1 transaction, ores_hot_bytes_v1 candidate_checkpoint,
        ores_hot_bytes_v1 candidate_ets_snapshot);
    ores_hot_status_v1 (*commit)(
        void *ctx, ores_hot_id_v1 transaction, uint64_t expected_route_epoch,
        uint64_t *out_route_epoch);
    ores_hot_status_v1 (*abort)(
        void *ctx, ores_hot_id_v1 transaction);
    ores_hot_status_v1 (*ack_ets_handoff)(
        void *ctx, ores_hot_id_v1 transaction, ores_hot_id_v1 receipt,
        ores_hot_id_v1 heir_actor, uint64_t *out_table_epoch);
    ores_hot_status_v1 (*retain)(
        void *ctx, ores_hot_id_v1 generation, ores_hot_id_v1 *out_lease);
    ores_hot_status_v1 (*release)(
        void *ctx, ores_hot_id_v1 lease);
    ores_hot_status_v1 (*retire)(
        void *ctx, ores_hot_id_v1 generation);
} ores_hot_api_v1;

/* Exported by a trusted adapter. Loader provides a zeroed out struct with
 * struct_size set. Runtime refuses unknown ABI versions and unsafe sizes.
 * The function table is immutable for the duration of its runtime lease.
 */
typedef ores_hot_status_v1 (*ores_hot_query_api_v1_fn)(
    uint32_t requested_version, ores_hot_api_v1 *inout_api);

/* Trusted adapters exporting this host-native API use this exact unmangled
 * name. Its binary ABI is architecture-specific; it is NOT a Wasm or wire ABI.
 * Explicit export maps / __declspec(dllexport) are required by the build.
 */
ores_hot_status_v1 ores_hot_query_api_v1(
    uint32_t requested_version, ores_hot_api_v1 *inout_api);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* ORES_HOT_LOAD_V1_H */
