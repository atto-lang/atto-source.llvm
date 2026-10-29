#pragma once
// Read-only Linux CGL host observation. NOT privileged RT admission or deadline proof.
#include <cstddef>
#include <optional>
#ifdef __linux__
#include <fcntl.h>
#include <sched.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#endif
namespace oreslang::cgl {
inline std::optional<bool> read_preempt_rt_flag(const char* path = "/sys/kernel/realtime") {
#ifdef __linux__
    if (path == nullptr) return std::nullopt;
    // A preflight must never hang on a FIFO, follow an attacker-supplied
    // symlink or allocate unbounded memory while parsing a sysfs-shaped flag.
    const int fd = ::open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return std::nullopt;
    struct stat st{};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        (void)::close(fd);
        return std::nullopt;
    }
    char raw[8]{};
    const ssize_t n = ::read(fd, raw, sizeof(raw));
    (void)::close(fd);
    if (n == 1 || (n == 2 && raw[1] == '\n')) {
        if (raw[0] == '1') return true;
        if (raw[0] == '0') return false;
    }
#else
    (void)path;
#endif
    return std::nullopt;
}
struct HostProbe {
    bool linux = false;
    std::size_t allowed_cpus = 0;
    bool monotonic_clock = false;
    int scheduler_policy = -1;
    std::optional<bool> preempt_rt;
    bool compatible(bool require_verified_preempt_rt = false) const noexcept {
        return linux && allowed_cpus > 0 && monotonic_clock && scheduler_policy >= 0 &&
               (!require_verified_preempt_rt || preempt_rt == std::optional<bool>{true});
    }
};
inline HostProbe probe_host() {
    HostProbe result;
#ifdef __linux__
    result.linux = true;
    cpu_set_t mask;
    CPU_ZERO(&mask);
    if (sched_getaffinity(0, sizeof(mask), &mask) == 0)
        result.allowed_cpus = static_cast<std::size_t>(CPU_COUNT(&mask));
    timespec resolution{};
    result.monotonic_clock = clock_getres(CLOCK_MONOTONIC, &resolution) == 0;
    result.scheduler_policy = sched_getscheduler(0);
    result.preempt_rt = read_preempt_rt_flag();
#endif
    return result;
}
} // namespace oreslang::cgl
