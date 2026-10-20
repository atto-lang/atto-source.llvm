#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Logical execution identity is deliberately distinct from allocation/heap
 * identity and from physical carrier identity.
 *
 * The structure is fixed-width and pointer-free so large runtime populations
 * can be stored as AoS today and migrated to SoA/AoSoA stores without changing
 * the ABI or leaking native pointers into guest-visible state.
 */
typedef enum {
    ORES_ALLOCATION_DOMAIN_NONE = 0,
    ORES_ALLOCATION_DOMAIN_ACTOR = 1,
    ORES_ALLOCATION_DOMAIN_TASK = 2,
    ORES_ALLOCATION_DOMAIN_RUNTIME = 3
} OresNativeAllocationDomainKind;

typedef struct {
    uint64_t kind;
    uint64_t domain;
    uint64_t owner;
    uint64_t generation;
} OresNativeAllocationDomainRef;

typedef struct {
    uint64_t runtime_domain;
    uint64_t execution;
} OresNativeExecutionId;

typedef struct {
    OresNativeExecutionId id;
    OresNativeAllocationDomainRef allocation;
    /*
     * Reserved logical links for compiler-lowered continuation ancestry.
     * Zero means absent. These are IDs/handles, never raw stack/heap pointers.
     */
    uint64_t parent_execution;
    uint64_t continuation;
} OresNativeExecutionContext;

#ifdef __cplusplus
}
#endif
