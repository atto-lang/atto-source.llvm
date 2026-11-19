#pragma once
/* Native host-side CPU admission hints, not a freestanding core dependency.
 * No syscall is made in a carrier hot path. Explicit pthread placement is
 * intentionally separate: processor capacity is not exclusive ownership. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>

static inline size_t ores_native_cpu_quota(const char* quota, const char* period) {
    if (!quota || !period || quota[0] == '-') return 0;
    char *qend = NULL, *pend = NULL;
    unsigned long long q = strtoull(quota, &qend, 10);
    unsigned long long p = strtoull(period, &pend, 10);
    if (qend == quota || pend == period || q == 0 || p == 0) return 0;
    while (*qend == ' ' || *qend == '\n' || *qend == '\t') ++qend;
    while (*pend == ' ' || *pend == '\n' || *pend == '\t') ++pend;
    if (*qend || *pend) return 0;
    unsigned long long cores = q / p + (q % p != 0);
    return cores > SIZE_MAX ? SIZE_MAX : (size_t)cores;
}

static inline size_t ores_native_cpu_max(const char* text) {
    if (!text || !*text || strncmp(text, "max", 3) == 0) return 0;
    char quota[64] = {0}, period[64] = {0}, extra[2] = {0};
    if (sscanf(text, "%63s %63s %1s", quota, period, extra) != 2) return 0;
    return ores_native_cpu_quota(quota, period);
}

/* Count sorted, nonoverlapping Linux cpuset ranges. Invalid sets are ignored. */
static inline size_t ores_native_cpu_set(const char* value) {
    if (!value || !*value) return 0;
    const char* p = value;
    unsigned long long previous = 0, count = 0;
    int first = 1;
    for (;;) {
        char* end = NULL;
        unsigned long long lo = strtoull(p, &end, 10);
        if (p == end || (*p == '-') || (!first && lo <= previous)) return 0;
        p = end;
        unsigned long long hi = lo;
        if (*p == '-') {
            p++;
            hi = strtoull(p, &end, 10);
            if (end == p || hi < lo) return 0;
            p = end;
        }
        if (hi >= UINT32_MAX || count > SIZE_MAX - (hi - lo + 1)) return 0;
        count += hi - lo + 1;
        previous = hi;
        first = 0;
        if (*p == ',') { ++p; continue; }
        while (*p == ' ' || *p == '\n' || *p == '\t') ++p;
        return *p == '\0' ? (size_t)count : 0;
    }
}

static inline int ores_native_cpu_read(const char* path, char* buf, size_t length) {
    if (length < 2) return 0;
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    int ok = fgets(buf, (int)length, f) != NULL;
    fclose(f);
    return ok;
}

static inline size_t ores_native_cpu_effective(void) {
    long online = sysconf(_SC_NPROCESSORS_ONLN);
    size_t budget = online > 0 ? (size_t)online : 1;
#if defined(__linux__)
    char buf[256] = {0};
    size_t cpuset = 0, quota = 0;
    if (ores_native_cpu_read("/sys/fs/cgroup/cpuset.cpus.effective", buf, sizeof buf))
        cpuset = ores_native_cpu_set(buf);
    if (!cpuset && ores_native_cpu_read("/sys/fs/cgroup/cpuset/cpuset.cpus", buf, sizeof buf))
        cpuset = ores_native_cpu_set(buf);
    if (ores_native_cpu_read("/sys/fs/cgroup/cpu.max", buf, sizeof buf))
        quota = ores_native_cpu_max(buf);
    if (!quota) {
        char q[64] = {0}, p[64] = {0};
        if (ores_native_cpu_read("/sys/fs/cgroup/cpu/cpu.cfs_quota_us", q, sizeof q)
            && ores_native_cpu_read("/sys/fs/cgroup/cpu/cpu.cfs_period_us", p, sizeof p))
            quota = ores_native_cpu_quota(q, p);
    }
    if (cpuset && cpuset < budget) budget = cpuset;
    if (quota && quota < budget) budget = quota;
#endif
    return budget ? budget : 1;
}

/* Four independently isolated scheduler queues need >=1 worker each.
 * Requested counts (nonzero) are preserved; CPU budget is a hint only
 * when the host elects this opt-in API. A quota below four necessarily
 * overcommits until there is a safe cross-domain nonparking CPU broker. */
static inline void ores_native_cpu_distribute(
    size_t budget, const size_t requested[4], size_t result[4]) {
    const size_t priority[4] = {1, 2, 3, 0};
    size_t taken = 0;
    for (size_t i = 0; i < 4; ++i) {
        result[i] = requested[i] ? requested[i] : 1;
        if (taken <= SIZE_MAX - result[i]) taken += result[i];
        else taken = SIZE_MAX;
    }
    if (budget < taken) return;
    size_t additional = budget - taken;
    while (additional) {
        int assigned = 0;
        for (size_t k = 0; k < 4 && additional; ++k) {
            size_t i = priority[k];
            if (!requested[i] && result[i] < 128) {
                ++result[i]; --additional; assigned = 1;
            }
        }
        if (!assigned) break;
    }
}
