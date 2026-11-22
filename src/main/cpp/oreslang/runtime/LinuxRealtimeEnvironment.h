#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Linux deployment evidence for a hard-real-time Oreslang domain.
 *
 * This layer is intentionally separate from the portable scheduler. A
 * successful call proves only that this process established
 * mlockall(MCL_CURRENT|MCL_FUTURE) through Oreslang. It does not prove CPU/IRQ
 * isolation, SCHED_FIFO privileges, WCET, or absence of external munlockall.
 */
int ores_linux_realtime_lock_process_memory(void);
int ores_linux_realtime_memory_lock_established(void);
int ores_linux_realtime_unlock_process_memory(void);

#ifdef __cplusplus
}
#endif
