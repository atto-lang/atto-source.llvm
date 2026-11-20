#include "oreslang/runtime/LinuxRealtimeEnvironment.h"
#include <stdatomic.h>

#if defined(__linux__)
#include <sys/mman.h>
#endif

static _Atomic int ores_memory_lock_established;

int ores_linux_realtime_lock_process_memory(void) {
#if defined(__linux__)
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        atomic_store_explicit(
            &ores_memory_lock_established, 0, memory_order_release);
        return 0;
    }
    atomic_store_explicit(
        &ores_memory_lock_established, 1, memory_order_release);
    return 1;
#else
    atomic_store_explicit(
        &ores_memory_lock_established, 0, memory_order_release);
    return 0;
#endif
}

int ores_linux_realtime_memory_lock_established(void) {
    return atomic_load_explicit(
        &ores_memory_lock_established, memory_order_acquire);
}

int ores_linux_realtime_unlock_process_memory(void) {
#if defined(__linux__)
    if (!ores_linux_realtime_memory_lock_established()) return 0;
    if (munlockall() != 0) return 0;
    atomic_store_explicit(
        &ores_memory_lock_established, 0, memory_order_release);
    return 1;
#else
    return 0;
#endif
}
