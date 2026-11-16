#include "oreslang/runtime/LinuxRealtimeEnvironment.h"
#include <stdlib.h>

static void require(int ok) { if (!ok) abort(); }

int main(void) {
    require(!ores_linux_realtime_memory_lock_established());

    /* CI containers commonly lack enough RLIMIT_MEMLOCK/CAP_IPC_LOCK. Both
     * outcomes are valid; the important property is that failure never
     * manufactures positive deployment evidence. */
    int locked = ores_linux_realtime_lock_process_memory();
    if (locked) {
        require(ores_linux_realtime_memory_lock_established());
        require(ores_linux_realtime_unlock_process_memory());
        require(!ores_linux_realtime_memory_lock_established());
    } else {
        require(!ores_linux_realtime_memory_lock_established());
    }
    return 0;
}
