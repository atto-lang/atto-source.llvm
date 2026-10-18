/* Cortex-M firmware entrypoint for ACTUAL LLVM-generated Oreslang actors.
 * NO native C actor constructor or message callback may be supplied here.
 * Production actor implementation MUST be resolved from compiled guest IR.
 */
#include "oreslang/runtime/FreestandingSourceActor.h"
#include <stdint.h>
extern int64_t oreslang_cortexm_bootstrap(void);
static volatile uint32_t bss_witness;
static volatile uint32_t data_witness = UINT32_C(0x63ade291);
int firmware_main(void) {
    if (bss_witness != 0 || data_witness != UINT32_C(0x63ade291)) return 70;
    if (oreslang_cortexm_bootstrap() != 43) return 71;
    OresFreestandingActorStats stats;
    ores_freestanding_source_stats(&stats);
    if (stats.spawned != 2 || stats.retired != 2 || stats.delivered != 4 ||
        stats.last_result[0] != 83 || stats.last_result[1] != 19) return 72;
#ifdef NEGATIVE_GUEST
    return 99;
#else
    return 0;
#endif
}
