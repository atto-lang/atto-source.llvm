#pragma once
// Observation-only CGL Linux host gate. This DOES NOT reserve CPUs, set RT
// priority, bound wakeup latency, or authorize use of a hard-realtime actor.
#include <cstddef>
#include <fstream>
#include <optional>
#include <string>
#ifdef __linux__
#include <sched.h>
#include <time.h>
#endif

namespace oreslang::cgl {
inline std::optional<bool> read_preempt_rt_flag(const char* path = "/sys/kernel/realtime") {
    std::ifstream in(path);
    std::string value;
    std::string trailing;
    if (!(in >> value) || (in >> trailing)) return std::nullopt;
    if (value == "1") return true;
    if (value == "0") return false;
    return std::nullopt;
}

struct HostProbe {
    bool linux = false;
    std::size_t allowed_cpus = 0;
    bool monotonic_clock = false;
    int scheduler_policy = -1;
    std::optional<bool> preempt_rt;

    bool compatible(bool require_verified_preempt_rt = false) const noexcept {
        return linux && allowed_cpus > 0 && monotonic_clock &&
               scheduler_policy >= 0 &&
               (!require_verified_preempt_rt || preempt_rt == std::optional<bool>{true});
    }
};

inline HostProbe probe_host() {
    HostProbe result;
#ifdef __linux__
    result.linux = true;
    cpu_set_t mask;
    CPU_ZERO(&mask);
    if (sched_getaffinity(0, sizeof(mask), &mask) == 0) {
        result.allowed_cpus = static_cast<std::size_t>(CPU_COUNT(&mask));
    }
    timespec resolution{};
    result.monotonic_clock = clock_getres(CLOCK_MONOTONIC, &resolution) == 0;
    result.scheduler_policy = sched_getscheduler(0);
    result.preempt_rt = read_preempt_rt_flag();
#endif
    return result;
}
} // namespace oreslang::cgl
