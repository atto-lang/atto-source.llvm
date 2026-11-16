/*
 * UHL-1 native host ABI adapter -- conservative admission state.
 *
 * The current LLVM native code-slot and source PIC test infrastructure does
 * NOT satisfy this complete supervisor+ETS transaction interface. Therefore
 * this adapter advertises ZERO features and NEVER stages an image. It provides
 * an explicit C11 ABI to prevent a host incorrectly assuming support.
 *
 * Trusted-host ONLY. Never call this API with untrusted guest pointers.
 * Source of normative declaration: oreslang-common-source
 * contracts/v1/hot-load-abi/ores_hot_load_v1.h (UHL-1 draft).
 */
#include "ores_hot_load_v1.h"

#include <stddef.h>

static ores_hot_status_v1 unsupported_stage(
    void *ctx, ores_hot_id_v1 authority, ores_hot_bytes_v1 manifest,
    ores_hot_id_v1 *out_txn) {
    (void)ctx; (void)authority; (void)manifest; (void)out_txn;
    return ORES_HOT_UNSUPPORTED;
}

static ores_hot_status_v1 unsupported_prepare(
    void *ctx, ores_hot_id_v1 txn, ores_hot_bytes_v1 plan) {
    (void)ctx; (void)txn; (void)plan;
    return ORES_HOT_UNSUPPORTED;
}

static ores_hot_status_v1 unsupported_freeze(
    void *ctx, ores_hot_id_v1 txn, uint64_t epoch) {
    (void)ctx; (void)txn; (void)epoch;
    return ORES_HOT_UNSUPPORTED;
}

static ores_hot_status_v1 unsupported_quiesce(
    void *ctx, ores_hot_id_v1 txn) {
    (void)ctx; (void)txn;
    return ORES_HOT_UNSUPPORTED;
}

static ores_hot_status_v1 unsupported_checkpoint(
    void *ctx, ores_hot_id_v1 txn, uint8_t *buffer,
    uint64_t capacity, uint64_t *out_length) {
    (void)ctx; (void)txn; (void)buffer; (void)capacity; (void)out_length;
    return ORES_HOT_UNSUPPORTED;
}

static ores_hot_status_v1 unsupported_migrate(
    void *ctx, ores_hot_id_v1 txn, ores_hot_bytes_v1 checkpoint,
    ores_hot_bytes_v1 table_snapshot) {
    (void)ctx; (void)txn; (void)checkpoint; (void)table_snapshot;
    return ORES_HOT_UNSUPPORTED;
}

static ores_hot_status_v1 unsupported_commit(
    void *ctx, ores_hot_id_v1 txn, uint64_t epoch, uint64_t *out_epoch) {
    (void)ctx; (void)txn; (void)epoch; (void)out_epoch;
    return ORES_HOT_UNSUPPORTED;
}

static ores_hot_status_v1 unsupported_abort(
    void *ctx, ores_hot_id_v1 txn) {
    (void)ctx; (void)txn;
    return ORES_HOT_UNSUPPORTED;
}

static ores_hot_status_v1 unsupported_ack_ets(
    void *ctx, ores_hot_id_v1 txn, ores_hot_id_v1 receipt,
    ores_hot_id_v1 heir, uint64_t *out_epoch) {
    (void)ctx; (void)txn; (void)receipt; (void)heir; (void)out_epoch;
    return ORES_HOT_UNSUPPORTED;
}

static ores_hot_status_v1 unsupported_retain(
    void *ctx, ores_hot_id_v1 generation, ores_hot_id_v1 *out_lease) {
    (void)ctx; (void)generation; (void)out_lease;
    return ORES_HOT_UNSUPPORTED;
}

static ores_hot_status_v1 unsupported_release(
    void *ctx, ores_hot_id_v1 lease) {
    (void)ctx; (void)lease;
    return ORES_HOT_UNSUPPORTED;
}

static ores_hot_status_v1 unsupported_retire(
    void *ctx, ores_hot_id_v1 generation) {
    (void)ctx; (void)generation;
    return ORES_HOT_UNSUPPORTED;
}

static ores_hot_status_v1 query_capabilities(
    void *ctx, ores_hot_capabilities_v1 *out) {
    (void)ctx;
    if (out == NULL) return ORES_HOT_BAD_HANDLE;
    if (out->struct_size != sizeof(*out)
        || (out->abi_version != 0u
            && out->abi_version != ORES_HOT_LOAD_ABI_VERSION))
        return ORES_HOT_BAD_ABI;
    out->struct_size = (uint32_t)sizeof(*out);
    out->abi_version = ORES_HOT_LOAD_ABI_VERSION;
    out->features = 0u; /* No source actor/ETS hot-load production support. */
    out->max_manifest_bytes = 0u;
    out->max_checkpoint_bytes = 0u;
    out->max_live_generations = 0u;
    return ORES_HOT_OK;
}

/*
 * Exact in-process C symbol. No side effects or guest execution during
 * negotiation. A bad version/size leaves the function table untouched.
 * The ABI adapter may be replaced by a tested runtime-backed implementation
 * only after one authoritative upgrade owner and real source/ETS receipts.
 */
ores_hot_status_v1 ores_hot_query_api_v1(
    uint32_t requested_version, ores_hot_api_v1 *inout_api) {
    if (inout_api == NULL) return ORES_HOT_BAD_HANDLE;
    if (requested_version != ORES_HOT_LOAD_ABI_VERSION
        || inout_api->struct_size != sizeof(*inout_api)
        || (inout_api->abi_version != 0u
            && inout_api->abi_version != requested_version))
        return ORES_HOT_BAD_ABI;

    inout_api->struct_size = (uint32_t)sizeof(*inout_api);
    inout_api->abi_version = ORES_HOT_LOAD_ABI_VERSION;
    inout_api->trusted_runtime_context = NULL;
    inout_api->query_capabilities = query_capabilities;
    inout_api->stage = unsupported_stage;
    inout_api->prepare = unsupported_prepare;
    inout_api->freeze = unsupported_freeze;
    inout_api->quiesce = unsupported_quiesce;
    inout_api->checkpoint = unsupported_checkpoint;
    inout_api->migrate = unsupported_migrate;
    inout_api->commit = unsupported_commit;
    inout_api->abort = unsupported_abort;
    inout_api->ack_ets_handoff = unsupported_ack_ets;
    inout_api->retain = unsupported_retain;
    inout_api->release = unsupported_release;
    inout_api->retire = unsupported_retire;
    return ORES_HOT_OK;
}
