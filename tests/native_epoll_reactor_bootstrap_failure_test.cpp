#include <sys/eventfd.h>
#include <sys/epoll.h>
#include <atomic>
#include <cerrno>
#include <dirent.h>
#include <iostream>
#include <stdexcept>
#include <system_error>

// Interpose only this translation unit's header calls. Do not modify
// process-wide libc symbols or export the test hook into the runtime.
static std::atomic<int> call_count{0};
static std::atomic<int> injected_call{0};
static std::atomic<int> injected_errno{EMFILE};

extern "C" int test_eventfd(unsigned int value, int flags) noexcept {
    const int n = call_count.fetch_add(1) + 1;
    if (n == injected_call.load()) {
        errno = injected_errno.load();
        return -1;
    }
    return ::eventfd(value, flags);
}

#define eventfd test_eventfd
#include "oreslang/runtime/NativeEpollReactor.hpp"
#undef eventfd

using oreslang::runtime::NativeEpollReactor;

static void expect(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(reason);
}

static unsigned open_fd_count() {
    DIR* listing = ::opendir("/proc/self/fd");
    expect(listing != nullptr, "procfs FD inventory unavailable");
    unsigned count = 0;
    while (::readdir(listing)) ++count;
    ::closedir(listing);
    return count;
}

int main() {
    const auto baseline = open_fd_count();

    call_count.store(0);
    injected_call.store(1);
    injected_errno.store(EMFILE);
    try {
        NativeEpollReactor should_fail;
        expect(false, "wakeup eventfd failure was not propagated");
    } catch (const std::system_error& failure) {
        expect(failure.code().value() == EMFILE, "lost wakeup errno");
    }
    expect(call_count.load() == 1, "allocated completion eventfd after wakeup failure");
    expect(open_fd_count() == baseline, "FD leaked after wakeup eventfd failure");

    call_count.store(0);
    injected_call.store(2);
    injected_errno.store(ENFILE);
    try {
        NativeEpollReactor should_fail;
        expect(false, "completion eventfd failure was not propagated");
    } catch (const std::system_error& failure) {
        expect(failure.code().value() == ENFILE, "lost completion errno");
    }
    expect(call_count.load() == 2, "completion eventfd failure was not exercised");
    expect(open_fd_count() == baseline, "FD leaked after completion eventfd failure");

    call_count.store(0);
    injected_call.store(0);
    {
        NativeEpollReactor healthy;
        expect(call_count.load() == 2, "expected two successful eventfd allocations");
        expect(healthy.start(), "healthy reactor failed to start");
        healthy.stop();
    }
    expect(open_fd_count() == baseline, "FD leaked after healthy reactor teardown");
    std::cout << "PASS eventfd startup failure errno/cleanup and healthy teardown\n";
    return 0;
}
