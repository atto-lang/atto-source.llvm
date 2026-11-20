/* Exact C11/C++17 ABI linkage and fail-closed host-adapter test.
 * SOURCE-FREE and HOST-ONLY; it never compiles/executes Oreslang guest code.
 */
#include "oreslang/runtime/ores_hot_load_v1.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void all_operations_refuse(
    const ores_hot_api_v1 *api) {
    const ores_hot_id_v1 fake = {UINT64_C(7), UINT64_C(9)};
    const uint8_t manifest[3] = {1u, 2u, 3u};
    const ores_hot_bytes_v1 bytes = {manifest, 3u};
    ores_hot_id_v1 out = {UINT64_C(111), UINT64_C(222)};
    uint64_t epoch = UINT64_C(37), length = UINT64_C(38);
    uint8_t buffer[4] = {0xaau, 0xbbu, 0xccu, 0xddu};
    void *const ctx = api->trusted_runtime_context;

    assert(api->stage(ctx, fake, bytes, &out) == ORES_HOT_UNSUPPORTED);
    assert(out.high == 111u && out.low == 222u);
    assert(api->prepare(ctx, fake, bytes) == ORES_HOT_UNSUPPORTED);
    assert(api->freeze(ctx, fake, 1u) == ORES_HOT_UNSUPPORTED);
    assert(api->quiesce(ctx, fake) == ORES_HOT_UNSUPPORTED);
    assert(api->checkpoint(ctx, fake, buffer, sizeof(buffer), &length)
           == ORES_HOT_UNSUPPORTED);
    assert(length == 38u && buffer[0] == 0xaau && buffer[3] == 0xddu);
    assert(api->migrate(ctx, fake, bytes, bytes) == ORES_HOT_UNSUPPORTED);
    assert(api->commit(ctx, fake, 1u, &epoch) == ORES_HOT_UNSUPPORTED);
    assert(epoch == 37u);
    assert(api->abort(ctx, fake) == ORES_HOT_UNSUPPORTED);
    assert(api->ack_ets_handoff(ctx, fake, fake, fake, &epoch)
           == ORES_HOT_UNSUPPORTED);
    assert(epoch == 37u);
    assert(api->retain(ctx, fake, &out) == ORES_HOT_UNSUPPORTED);
    assert(out.high == 111u && out.low == 222u);
    assert(api->release(ctx, fake) == ORES_HOT_UNSUPPORTED);
    assert(api->retire(ctx, fake) == ORES_HOT_UNSUPPORTED);
}

int main(void) {
    ores_hot_api_v1 api = {0};
    api.struct_size = (uint32_t)sizeof(api);
    api.abi_version = ORES_HOT_LOAD_ABI_VERSION;

    assert(ores_hot_query_api_v1(1u, NULL) == ORES_HOT_BAD_HANDLE);

    ores_hot_api_v1 snapshot = api;
    assert(ores_hot_query_api_v1(2u, &api) == ORES_HOT_BAD_ABI);
    assert(memcmp(&api, &snapshot, sizeof(api)) == 0);
    api.struct_size--;
    snapshot = api;
    assert(ores_hot_query_api_v1(1u, &api) == ORES_HOT_BAD_ABI);
    assert(memcmp(&api, &snapshot, sizeof(api)) == 0);
    api.struct_size = (uint32_t)sizeof(api);
    api.abi_version = 2u;
    snapshot = api;
    assert(ores_hot_query_api_v1(1u, &api) == ORES_HOT_BAD_ABI);
    assert(memcmp(&api, &snapshot, sizeof(api)) == 0);

    api.abi_version = 0u; /* explicitly accepted version negotiation */
    assert(ores_hot_query_api_v1(ORES_HOT_LOAD_ABI_VERSION, &api)
           == ORES_HOT_OK);
    assert(api.abi_version == ORES_HOT_LOAD_ABI_VERSION);
    assert(api.struct_size == sizeof(api));
    assert(api.trusted_runtime_context == NULL);
    assert(api.query_capabilities != NULL);
    assert(api.stage != NULL && api.prepare != NULL);
    assert(api.freeze != NULL && api.quiesce != NULL);
    assert(api.checkpoint != NULL && api.migrate != NULL);
    assert(api.commit != NULL && api.abort != NULL);
    assert(api.ack_ets_handoff != NULL);
    assert(api.retain != NULL && api.release != NULL && api.retire != NULL);

    ores_hot_capabilities_v1 caps = {0};
    assert(api.query_capabilities(NULL, NULL) == ORES_HOT_BAD_HANDLE);
    caps.struct_size = (uint32_t)sizeof(caps) - 1u;
    assert(api.query_capabilities(NULL, &caps) == ORES_HOT_BAD_ABI);
    caps.struct_size = (uint32_t)sizeof(caps);
    caps.abi_version = 2u;
    assert(api.query_capabilities(NULL, &caps) == ORES_HOT_BAD_ABI);
    caps.abi_version = 0u;
    assert(api.query_capabilities(NULL, &caps) == ORES_HOT_OK);
    assert(caps.abi_version == ORES_HOT_LOAD_ABI_VERSION);
    assert(caps.features == 0u);
    assert(caps.max_manifest_bytes == 0u);
    assert(caps.max_checkpoint_bytes == 0u);
    assert(caps.max_live_generations == 0u);
    assert(0u == (caps.features &
        (ORES_HOT_FEAT_CODE_ONLY | ORES_HOT_FEAT_STATE_MIGRATION |
         ORES_HOT_FEAT_SUPERVISED_SUBTREE | ORES_HOT_FEAT_ETS_HEIR |
         ORES_HOT_FEAT_ETS_MIGRATION | ORES_HOT_FEAT_UNTRUSTED |
         ORES_HOT_FEAT_NATIVE_UNLOAD | ORES_HOT_MODE_AOT_TO_AOT |
         ORES_HOT_MODE_AOT_TO_JIT | ORES_HOT_MODE_JIT_TO_AOT |
         ORES_HOT_MODE_JIT_TO_JIT)));

    for (int iteration = 0; iteration != 10000; ++iteration)
        all_operations_refuse(&api);

    puts("UHL1_C_ABI_HOST_ONLY_PASS=10000");
    return 0;
}
