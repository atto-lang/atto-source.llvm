#include "oreslang/runtime/SourceRecoveryLeaseV1.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) abort(); } while (0)
int main(void) {
    OresSourceRecoveryLeaseV1 lease = {0};
    for (uint64_t i = 1; i <= 1000000; ++i) {
        const size_t slot = (size_t)(i % 63);
        CHECK(ores_source_recovery_can_close(&lease, 0));
        CHECK(!ores_source_recovery_can_close(&lease, 1));
        CHECK(!ores_source_recovery_claim(&lease, slot, 0, i));
        CHECK(!ores_source_recovery_claim(&lease, slot, i, 0));
        CHECK(!ores_source_recovery_matches(&lease, slot, i, i + 1));
        CHECK(ores_source_recovery_claim(&lease, slot, i, i + 1));
        CHECK(ores_source_recovery_matches(&lease, slot, i, i + 1));
        CHECK(!ores_source_recovery_matches(&lease, (slot + 1) % 63, i, i + 1));
        CHECK(!ores_source_recovery_matches(&lease, slot, i + 1, i + 1));
        CHECK(!ores_source_recovery_matches(&lease, slot, i, i));
        CHECK(!ores_source_recovery_can_close(&lease, 0));
        CHECK(!ores_source_recovery_claim(&lease, slot, i, i + 1));
        CHECK(!ores_source_recovery_release(&lease, (slot + 1) % 63, i, i + 1));
        CHECK(!ores_source_recovery_release(&lease, slot, i + 1, i + 1));
        CHECK(!ores_source_recovery_release(&lease, slot, i, i));
        CHECK(!ores_source_recovery_can_close(&lease, 0));
        CHECK(ores_source_recovery_release(&lease, slot, i, i + 1));
        CHECK(!ores_source_recovery_matches(&lease, slot, i, i + 1));
        CHECK(!ores_source_recovery_release(&lease, slot, i, i + 1));
        CHECK(ores_source_recovery_can_close(&lease, 0));
        CHECK(!ores_source_recovery_can_close(&lease, 1));
    }
    /* Full-width identity boundary; comparison must not wrap. */
    CHECK(ores_source_recovery_claim(&lease, 0, UINT64_MAX, UINT64_MAX));
    CHECK(ores_source_recovery_matches(&lease, 0, UINT64_MAX, UINT64_MAX));
    CHECK(!ores_source_recovery_matches(&lease, 0, UINT64_MAX - 1, UINT64_MAX));
    CHECK(!ores_source_recovery_matches(&lease, 0, UINT64_MAX, UINT64_MAX - 1));
    CHECK(!ores_source_recovery_release(&lease, 0, UINT64_MAX - 1, UINT64_MAX));
    CHECK(!ores_source_recovery_release(&lease, 0, UINT64_MAX, UINT64_MAX - 1));
    CHECK(!ores_source_recovery_release(&lease, 0, 0, 0));
    CHECK(!ores_source_recovery_can_close(&lease, 0));
    CHECK(ores_source_recovery_release(&lease, 0, UINT64_MAX, UINT64_MAX));
    CHECK(!ores_source_recovery_matches(&lease, 0, UINT64_MAX, UINT64_MAX));
    CHECK(ores_source_recovery_can_close(&lease, 0));
    puts("PASS RECOVERY_LEASE_CYCLES=1000000 STALE_RELEASE_DENIED=1 IDLE_CLOSE_FENCED=1");
}
