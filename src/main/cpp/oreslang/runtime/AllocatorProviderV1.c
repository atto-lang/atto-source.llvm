#include "oreslang/runtime/AllocatorProviderV1.h"

#include <stdint.h>

static OresAllocatorProviderV1* current_provider = 0;

static void wipe_bytes(void* data, size_t count) {
    volatile uint8_t* bytes = (volatile uint8_t*)data;
    for (size_t i = 0; i < count; ++i) bytes[i] = 0;
}

static int power_of_two(size_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}

static int provider_zero_initialized(
        const OresAllocatorProviderV1* provider) {
    return provider &&
        provider->base == 0 &&
        provider->capacity == 0 &&
        provider->used == 0 &&
        provider->high_water == 0 &&
        provider->generation == 0 &&
        provider->initialized == 0;
}

static int backing_overlaps_provider(
        const OresAllocatorProviderV1* provider,
        const void* memory,
        size_t capacity) {
    const uintptr_t provider_begin = (uintptr_t)provider;
    const uintptr_t memory_begin = (uintptr_t)memory;
    if (provider_begin > UINTPTR_MAX - sizeof(*provider) ||
            capacity > UINTPTR_MAX - memory_begin)
        return 1;
    const uintptr_t provider_end =
        provider_begin + sizeof(*provider);
    const uintptr_t memory_end = memory_begin + capacity;
    return memory_begin < provider_end &&
        provider_begin < memory_end;
}

static int provider_state_valid(
        const OresAllocatorProviderV1* provider) {
    return provider &&
        provider->initialized == UINT32_C(0x4f524531) &&
        provider->base &&
        provider->capacity != 0 &&
        provider->generation != 0 &&
        provider->used <= provider->capacity &&
        provider->high_water <= provider->capacity &&
        provider->used <= provider->high_water &&
        !backing_overlaps_provider(
            provider, provider->base, provider->capacity);
}

OresAllocatorProviderV1Status ores_allocator_provider_v1_init(
        OresAllocatorProviderV1* provider, void* memory, size_t capacity) {
    if (!provider || !memory || capacity == 0 ||
            current_provider == provider ||
            !provider_zero_initialized(provider) ||
            backing_overlaps_provider(provider, memory, capacity))
        return ORES_ALLOCATOR_PROVIDER_V1_INVALID;
    provider->base = (uint8_t*)memory;
    provider->capacity = capacity;
    provider->used = 0;
    provider->high_water = 0;
    provider->generation = 1;
    provider->initialized = UINT32_C(0x4f524531); /* "ORE1" */
    wipe_bytes(memory, capacity);
    return ORES_ALLOCATOR_PROVIDER_V1_OK;
}

void* ores_allocator_provider_v1_alloc(
        OresAllocatorProviderV1* provider, size_t bytes, size_t alignment) {
    if (!provider_state_valid(provider) ||
            bytes == 0 || !power_of_two(alignment) ||
            alignment > 64)
        return 0;
    if (provider->used > provider->capacity ||
            bytes > provider->capacity)
        return 0;

    const uintptr_t base = (uintptr_t)provider->base;
    const uintptr_t current = base + provider->used;
    if (current < base) return 0;
    const uintptr_t mask = (uintptr_t)alignment - 1u;
    if (current > UINTPTR_MAX - mask) return 0;
    const uintptr_t aligned = (current + mask) & ~mask;
    if (aligned < current) return 0;
    const size_t padding = (size_t)(aligned - current);
    if (padding > provider->capacity - provider->used ||
            bytes > provider->capacity - provider->used - padding)
        return 0;

    const size_t next = provider->used + padding + bytes;
    void* result = (void*)aligned;
    provider->used = next;
    if (provider->high_water < next) provider->high_water = next;
    wipe_bytes(result, bytes);
    return result;
}

OresAllocatorProviderV1Status ores_allocator_provider_v1_bind(
        OresAllocatorProviderV1* provider) {
    if (!provider_state_valid(provider) || current_provider)
        return ORES_ALLOCATOR_PROVIDER_V1_INVALID;
    current_provider = provider;
    return ORES_ALLOCATOR_PROVIDER_V1_OK;
}

OresAllocatorProviderV1Status ores_allocator_provider_v1_unbind(
        OresAllocatorProviderV1* provider) {
    if (!provider_state_valid(provider) ||
            current_provider != provider)
        return ORES_ALLOCATOR_PROVIDER_V1_INVALID;
    current_provider = 0;
    return ORES_ALLOCATOR_PROVIDER_V1_OK;
}

OresAllocatorProviderV1Status ores_allocator_provider_v1_reset(
        OresAllocatorProviderV1* provider) {
    if (!provider_state_valid(provider) ||
            current_provider == provider)
        return ORES_ALLOCATOR_PROVIDER_V1_INVALID;
    wipe_bytes(provider->base, provider->used);
    provider->used = 0;
    if (provider->generation == UINT32_MAX)
        provider->generation = 1;
    else
        ++provider->generation;
    return ORES_ALLOCATOR_PROVIDER_V1_OK;
}

OresAllocatorProviderV1* ores_allocator_provider_v1_current(void) {
    return current_provider;
}

size_t ores_allocator_provider_v1_used(
        const OresAllocatorProviderV1* provider) {
    return provider_state_valid(provider) ? provider->used : 0;
}

size_t ores_allocator_provider_v1_high_water(
        const OresAllocatorProviderV1* provider) {
    return provider_state_valid(provider) ? provider->high_water : 0;
}
