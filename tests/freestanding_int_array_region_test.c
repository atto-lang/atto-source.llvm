#include "oreslang/runtime/AllocatorProviderV1.h"
#include "oreslang/runtime/NativeIntArrayRegion.h"

#include <assert.h>
#include <stdint.h>

int main(void) {
    union {
        uint64_t align;
        uint8_t bytes[2048];
    } storage = {0};
    OresAllocatorProviderV1 provider = ORES_ALLOCATOR_PROVIDER_V1_ZERO_INIT;

    assert(ores_allocator_provider_v1_init(
        &provider, storage.bytes, sizeof(storage.bytes)) ==
        ORES_ALLOCATOR_PROVIDER_V1_OK);
    assert(ores_allocator_provider_v1_bind(&provider) ==
        ORES_ALLOCATOR_PROVIDER_V1_OK);

    OresNativeIntArrayRegion* region = 0;
    assert(oreslang_int_array_try_region_open(&region) ==
        ORES_NATIVE_ARRAY_OK);
    assert(region != 0);

    {
        const uint64_t lengths[1] = {1024};
        const int64_t values[1] = {7};
        OresNativeIntArrayHandle out[1] = {0};
        const OresNativeIntArrayStatus status =
            oreslang_int_array_try_prepare_filled_many(
                region, 1, lengths, values, out);
        assert(status == ORES_NATIVE_ARRAY_NO_MEMORY ||
            status == ORES_NATIVE_ARRAY_LIMIT);
        assert(out[0] == 0);
    }

    /*
     * The failed prepare above acquired a slot before reserve failed.
     * All 16 slots must still be available afterward; otherwise the current
     * staged handle leaked from the rollback path.
     */
    OresNativeIntArrayHandle handles[16] = {0};
    assert(oreslang_int_array_try_prepare_many(region, 16, handles) ==
        ORES_NATIVE_ARRAY_OK);
    for (unsigned i = 0; i < 16; ++i) {
        assert(handles[i] != 0);
        oreslang_int_array_drop(region, handles[i]);
    }

    oreslang_int_array_region_close(region);
    assert(ores_allocator_provider_v1_unbind(&provider) ==
        ORES_ALLOCATOR_PROVIDER_V1_OK);
    assert(ores_allocator_provider_v1_reset(&provider) ==
        ORES_ALLOCATOR_PROVIDER_V1_OK);
    assert(ores_allocator_provider_v1_used(&provider) == 0);

    return 0;
}
