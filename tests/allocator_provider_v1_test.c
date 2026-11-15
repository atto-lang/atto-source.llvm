#include "oreslang/runtime/AllocatorProviderV1.h"

#include <assert.h>
#include <stdint.h>

int main(void) {
    union {
        uint64_t align;
        uint8_t bytes[512];
    } storage = {0};
    OresAllocatorProviderV1 provider = {0};
    OresAllocatorProviderV1 other = {0};
    union {
        uint64_t align;
        uint8_t bytes[512];
    } other_storage = {0};

    {
        OresAllocatorProviderV1 dirty = {0};
        dirty.used = 1;
        assert(ores_allocator_provider_v1_init(
            &dirty, other_storage.bytes, sizeof(other_storage.bytes)) ==
            ORES_ALLOCATOR_PROVIDER_V1_INVALID);
        dirty.used = 0;
        dirty.generation = 1;
        assert(ores_allocator_provider_v1_init(
            &dirty, other_storage.bytes, sizeof(other_storage.bytes)) ==
            ORES_ALLOCATOR_PROVIDER_V1_INVALID);
    }

    assert(ores_allocator_provider_v1_init(
        &provider, storage.bytes, sizeof(storage.bytes)) ==
        ORES_ALLOCATOR_PROVIDER_V1_OK);
    assert(ores_allocator_provider_v1_used(&provider) == 0);
    assert(ores_allocator_provider_v1_high_water(&provider) == 0);

    void* first = ores_allocator_provider_v1_alloc(&provider, 7, 8);
    void* second = ores_allocator_provider_v1_alloc(&provider, 17, 16);
    assert(first != 0 && second != 0 && first != second);
    assert(((uintptr_t)first & 7u) == 0);
    assert(((uintptr_t)second & 15u) == 0);
    assert(ores_allocator_provider_v1_used(&provider) > 24);
    assert(ores_allocator_provider_v1_high_water(&provider) ==
        ores_allocator_provider_v1_used(&provider));

    assert(ores_allocator_provider_v1_init(
        &other, other_storage.bytes, sizeof(other_storage.bytes)) ==
        ORES_ALLOCATOR_PROVIDER_V1_OK);
    assert(ores_allocator_provider_v1_bind(&provider) ==
        ORES_ALLOCATOR_PROVIDER_V1_OK);
    assert(ores_allocator_provider_v1_current() == &provider);
    assert(ores_allocator_provider_v1_init(
        &provider, storage.bytes, sizeof(storage.bytes)) ==
        ORES_ALLOCATOR_PROVIDER_V1_INVALID);
    assert(ores_allocator_provider_v1_current() == &provider);
    assert(ores_allocator_provider_v1_bind(&other) ==
        ORES_ALLOCATOR_PROVIDER_V1_INVALID);
    assert(ores_allocator_provider_v1_reset(&provider) ==
        ORES_ALLOCATOR_PROVIDER_V1_INVALID);
    assert(ores_allocator_provider_v1_unbind(&provider) ==
        ORES_ALLOCATOR_PROVIDER_V1_OK);
    assert(ores_allocator_provider_v1_current() == 0);

    const size_t high = ores_allocator_provider_v1_high_water(&provider);
    assert(high > 0);
    assert(ores_allocator_provider_v1_reset(&provider) ==
        ORES_ALLOCATOR_PROVIDER_V1_OK);
    assert(ores_allocator_provider_v1_used(&provider) == 0);
    assert(ores_allocator_provider_v1_high_water(&provider) == high);
    assert(ores_allocator_provider_v1_init(
        &provider, storage.bytes, sizeof(storage.bytes)) ==
        ORES_ALLOCATOR_PROVIDER_V1_INVALID);
    assert(ores_allocator_provider_v1_alloc(
        &provider, sizeof(storage.bytes) + 1, 8) == 0);
    assert(ores_allocator_provider_v1_alloc(&provider, 8, 3) == 0);

    {
        union {
            uint64_t align;
            uint8_t bytes[sizeof(OresAllocatorProviderV1) + 64];
        } overlap = {0};
        OresAllocatorProviderV1* embedded =
            (OresAllocatorProviderV1*)overlap.bytes;
        assert(ores_allocator_provider_v1_init(
            embedded, overlap.bytes, sizeof(overlap.bytes)) ==
            ORES_ALLOCATOR_PROVIDER_V1_INVALID);
    }

    provider.used = provider.capacity + 1;
    assert(ores_allocator_provider_v1_bind(&provider) ==
        ORES_ALLOCATOR_PROVIDER_V1_INVALID);
    assert(ores_allocator_provider_v1_used(&provider) == 0);
    provider.used = 0;
    provider.high_water = provider.capacity + 1;
    assert(ores_allocator_provider_v1_bind(&provider) ==
        ORES_ALLOCATOR_PROVIDER_V1_INVALID);
    assert(ores_allocator_provider_v1_high_water(&provider) == 0);

    return 0;
}
