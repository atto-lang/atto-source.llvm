#ifndef ORESLANG_SOURCE_RECOVERY_LEASE_V1_H
#define ORESLANG_SOURCE_RECOVERY_LEASE_V1_H
#include <stddef.h>
#include <stdint.h>

/* Trusted outer-main lease, serialized by the source group's mutex.
 * No actor storage or scheduler authority is manufactured by this record. */
typedef struct {
    size_t slot;
    uint64_t generation;
    uint64_t correlation;
    int held;
} OresSourceRecoveryLeaseV1;

static inline int ores_source_recovery_claim(OresSourceRecoveryLeaseV1* lease,
        size_t slot, uint64_t generation, uint64_t correlation) {
    if (lease->held || !generation || !correlation) return 0;
    *lease = (OresSourceRecoveryLeaseV1){slot, generation, correlation, 1};
    return 1;
}

/* Pure identity comparison, under the same caller-owned mutex as claim. */
static inline int ores_source_recovery_matches(const OresSourceRecoveryLeaseV1* lease,
        size_t slot, uint64_t generation, uint64_t correlation) {
    return lease->held && lease->slot == slot
        && lease->generation == generation && lease->correlation == correlation;
}

static inline int ores_source_recovery_release(OresSourceRecoveryLeaseV1* lease,
        size_t slot, uint64_t generation, uint64_t correlation) {
    if (!ores_source_recovery_matches(lease, slot, generation, correlation)) return 0;
    *lease = (OresSourceRecoveryLeaseV1){0};
    return 1;
}

static inline int ores_source_recovery_can_close(
        const OresSourceRecoveryLeaseV1* lease, size_t live) {
    return live == 0 && !lease->held;
}
#endif
