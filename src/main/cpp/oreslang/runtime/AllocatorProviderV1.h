#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Freestanding allocator provider ABI v1.
 *
 * The embedding target owns the backing memory and must initialize/bind the
 * provider explicitly before entering guest code that requires Allocator.
 * Provider storage MUST be zero-initialized before its first init call; after
 * successful init, reuse is only through unbind + reset and init is one-shot.
 * No malloc/calloc/realloc/free, OS handle, TLS, thread, or hidden fallback is
 * part of this ABI. v1 is deliberately single-owner/single-carrier.
 */
typedef enum {
    ORES_ALLOCATOR_PROVIDER_V1_OK = 0,
    ORES_ALLOCATOR_PROVIDER_V1_INVALID = 1,
    ORES_ALLOCATOR_PROVIDER_V1_NO_MEMORY = 2
} OresAllocatorProviderV1Status;

typedef struct {
    uint8_t* base;
    size_t capacity;
    size_t used;
    size_t high_water;
    uint32_t generation;
    uint32_t initialized;
} OresAllocatorProviderV1;

#define ORES_ALLOCATOR_PROVIDER_V1_ZERO_INIT {0}

OresAllocatorProviderV1Status ores_allocator_provider_v1_init(
    OresAllocatorProviderV1* provider, void* memory, size_t capacity);

void* ores_allocator_provider_v1_alloc(
    OresAllocatorProviderV1* provider, size_t bytes, size_t alignment);

OresAllocatorProviderV1Status ores_allocator_provider_v1_bind(
    OresAllocatorProviderV1* provider);

OresAllocatorProviderV1Status ores_allocator_provider_v1_unbind(
    OresAllocatorProviderV1* provider);

OresAllocatorProviderV1Status ores_allocator_provider_v1_reset(
    OresAllocatorProviderV1* provider);

OresAllocatorProviderV1* ores_allocator_provider_v1_current(void);

size_t ores_allocator_provider_v1_used(
    const OresAllocatorProviderV1* provider);

size_t ores_allocator_provider_v1_high_water(
    const OresAllocatorProviderV1* provider);

#ifdef __cplusplus
}
#endif
