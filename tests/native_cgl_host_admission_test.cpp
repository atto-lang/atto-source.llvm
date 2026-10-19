#include "oreslang/runtime/CglHostAdmission.hpp"
#include <cassert>
#include <cstdlib>
#include <optional>
#include <string>
#ifdef __linux__
#include <unistd.h>
#endif

int main() {
    using oreslang::cgl::HostProbe;
    using oreslang::cgl::read_preempt_rt_flag;
    HostProbe probe = oreslang::cgl::probe_host();
#ifdef __linux__
    assert(probe.linux);
    // Host observation may fail under a constrained runner; never manufacture success.
    if (probe.compatible()) {
        assert(probe.allowed_cpus > 0);
        assert(probe.monotonic_clock);
        assert(probe.scheduler_policy >= 0);
    }
    char path[] = "/tmp/ores-cgl-flag-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    auto check = [&](const std::string& contents, std::optional<bool> expected) {
        assert(ftruncate(fd, 0) == 0);
        assert(lseek(fd, 0, SEEK_SET) == 0);
        assert(write(fd, contents.data(), contents.size()) ==
               static_cast<ssize_t>(contents.size()));
        assert(read_preempt_rt_flag(path) == expected);
    };
    check("1\n", true);
    check("0\n", false);
    check("1 trailing\n", std::nullopt);
    check("maybe\n", std::nullopt);
    assert(close(fd) == 0);
    assert(unlink(path) == 0);
#else
    assert(!probe.compatible());
#endif
    HostProbe synthetic{true, 2, true, 0, true};
    assert(synthetic.compatible());
    assert(synthetic.compatible(true));
    synthetic.preempt_rt = std::nullopt;
    assert(synthetic.compatible());
    assert(!synthetic.compatible(true));
    synthetic.preempt_rt = false;
    assert(!synthetic.compatible(true));
    synthetic.allowed_cpus = 0;
    assert(!synthetic.compatible());
    synthetic.allowed_cpus = 2;
    synthetic.monotonic_clock = false;
    assert(!synthetic.compatible());
    synthetic.monotonic_clock = true;
    synthetic.scheduler_policy = -1;
    assert(!synthetic.compatible());
    return 0;
}
