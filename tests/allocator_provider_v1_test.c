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

    // Existing native provider physically aligns granted addresses, bounded
    // to 64 bytes; Atto's padding calculator is NOT equivalent to this proof.
    void* aligned64 = ores_allocator_provider_v1_alloc(&provider, 31, 64);
    assert(aligned64 != 0 && (((uintptr_t)aligned64) & 63u) == 0);
    assert(ores_allocator_provider_v1_alloc(&provider, 1, 128) == 0);
    for (size_t i = 0; i < 7; ++i) ((uint8_t*)first)[i] = 0xA5;
    for (size_t i = 0; i < 17; ++i) ((uint8_t*)second)[i] = 0x5A;
    for (size_t i = 0; i < 31; ++i) ((uint8_t*)aligned64)[i] = 0xBC;


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
    // The provider reset scrubs issued bytes through volatile stores.
    // Test only accessible backing bytes; this does not prove every CPU
    // cache, register, copy or JIT/managed-memory alias was erased.
    for (size_t i = 0; i < 7; ++i) assert(((uint8_t*)first)[i] == 0);
    for (size_t i = 0; i < 17; ++i) assert(((uint8_t*)second)[i] == 0);
    for (size_t i = 0; i < 31; ++i) assert(((uint8_t*)aligned64)[i] == 0);

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
