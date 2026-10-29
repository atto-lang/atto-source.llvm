#include "oreslang/runtime/CglHostAdmission.hpp"
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#ifdef __linux__
#include <sys/stat.h>
#include <unistd.h>
#endif

static void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "CGL native host admission failure: %s\n", message);
        std::abort();
    }
}
int main() {
    using oreslang::cgl::HostProbe;
    using oreslang::cgl::read_preempt_rt_flag;
    const HostProbe probe = oreslang::cgl::probe_host();
#ifdef __linux__
    require(probe.linux, "Linux identification");
    require(probe.compatible(), "effective affinity/monotonic clock/scheduler unavailable");
    char path[] = "/tmp/ores-cgl-flag-XXXXXX";
    const int fd = mkstemp(path);
    require(fd >= 0, "tempfile creation");
    const auto check = [&](const std::string& contents, std::optional<bool> expected) {
        require(ftruncate(fd, 0) == 0, "truncate");
        require(lseek(fd, 0, SEEK_SET) == 0, "seek");
        require(write(fd, contents.data(), contents.size()) ==
                    static_cast<ssize_t>(contents.size()), "write");
        require(read_preempt_rt_flag(path) == expected, "fail-closed realtime flag");
    };
    check("1\n", true);
    check("0\n", false);
    check("1", true);
    check("0", false);
    check("1 trailing\n", std::nullopt);
    check(" 1\n", std::nullopt);
    check("1 \n", std::nullopt);
    check("1\n\n", std::nullopt);
    check("maybe\n", std::nullopt);
    check("\n", std::nullopt);
    check(std::string(4096, '1'), std::nullopt);
    require(read_preempt_rt_flag(nullptr) == std::nullopt, "null path");
    char link_path[] = "/tmp/ores-cgl-link-XXXXXX";
    const int link_fd = mkstemp(link_path);
    require(link_fd >= 0, "link path reservation");
    require(close(link_fd) == 0, "close link reservation");
    require(unlink(link_path) == 0, "unlink link reservation");
    check("1\n", true);
    require(symlink(path, link_path) == 0, "create symlink");
    require(read_preempt_rt_flag(link_path) == std::nullopt, "symlink must be rejected");
    require(unlink(link_path) == 0, "unlink symlink");
    require(close(fd) == 0, "close");
    require(unlink(path) == 0, "unlink");
    require(mkfifo(path, 0600) == 0, "mkfifo");
    require(read_preempt_rt_flag(path) == std::nullopt, "empty FIFO must fail without blocking");
    require(unlink(path) == 0, "unlink fifo");
#else
    require(!probe.compatible(), "non-Linux host must fail closed");
#endif
    HostProbe synthetic{true, 2, true, 0, true};
    require(synthetic.compatible(), "normal admission");
    require(synthetic.compatible(true), "verified RT admission");
    synthetic.preempt_rt = std::nullopt;
    require(synthetic.compatible() && !synthetic.compatible(true), "unknown RT");
    synthetic.preempt_rt = false;
    require(!synthetic.compatible(true), "disabled RT");
    synthetic.allowed_cpus = 0;
    require(!synthetic.compatible(), "zero CPUs");
    synthetic.allowed_cpus = 2;
    synthetic.monotonic_clock = false;
    require(!synthetic.compatible(), "nonmonotonic clock");
    synthetic.monotonic_clock = true;
    synthetic.scheduler_policy = -1;
    require(!synthetic.compatible(), "unverified scheduler policy");
    return 0;
}
