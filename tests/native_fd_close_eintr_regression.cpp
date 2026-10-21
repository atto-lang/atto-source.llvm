// Linux-only exact-source regression: close(2) may report EINTR AFTER the
// descriptor has been released. Never retry the numeric FD: it may have
// been reassigned to another thread. The trusted host ledger must not leak
// a slot on EINTR, and must fail closed on EBADF/unexpected close failures.
// Requires -Wl,--wrap=close,--wrap=read,--wrap=write. close executes the REAL
// kernel operation first before altering its userspace result; read/write
// inject EINTR before the real syscall so retry semantics are deterministic.
#include "oreslang/runtime/NativeFdAuthorityBroker.hpp"
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <unistd.h>

static std::atomic<int> injected_errno{0};
static std::atomic<unsigned> injected_calls{0};
static std::atomic<int> injected_read_errno{0};
static std::atomic<int> injected_write_errno{0};
static std::atomic<unsigned> injected_read_calls{0};
static std::atomic<unsigned> injected_write_calls{0};

extern "C" ssize_t __real_read(int fd, void* buffer, size_t count);
extern "C" ssize_t __wrap_read(int fd, void* buffer, size_t count) {
    const int artificial = injected_read_errno.exchange(0, std::memory_order_acq_rel);
    if (artificial != 0) {
        injected_read_calls.fetch_add(1, std::memory_order_relaxed);
        errno = artificial;
        return -1;
    }
    return __real_read(fd, buffer, count);
}
extern "C" ssize_t __real_write(int fd, const void* buffer, size_t count);
extern "C" ssize_t __wrap_write(int fd, const void* buffer, size_t count) {
    const int artificial = injected_write_errno.exchange(0, std::memory_order_acq_rel);
    if (artificial != 0) {
        injected_write_calls.fetch_add(1, std::memory_order_relaxed);
        errno = artificial;
        return -1;
    }
    return __real_write(fd, buffer, count);
}

extern "C" int __real_close(int fd);
extern "C" int __wrap_close(int fd) {
    const int real_result = __real_close(fd);
    const int artificial = injected_errno.exchange(0, std::memory_order_acq_rel);
    if (artificial != 0) {
        if (real_result != 0) std::abort();
        injected_calls.fetch_add(1, std::memory_order_relaxed);
        errno = artificial;
        return -1;
    }
    return real_result;
}
static void check(bool valid) { if (!valid) std::abort(); }
static void arm(int code) {
    check(injected_errno.load(std::memory_order_acquire) == 0);
    injected_errno.store(code, std::memory_order_release);
}
int main() {
    using oreslang::runtime::NativeFdAuthorityBroker;
    unsigned expected_calls = 0;
    // Linux explicitly guarantees that even EINTR releases the descriptor.
    for (int iteration = 0; iteration < 128; ++iteration) {
        NativeFdAuthorityBroker broker(1);
        auto key = broker.mint_eventfd(71);
        check(key && broker.revoke(*key, 71));
        arm(EINTR);
        check(broker.confirm_reclaimed(*key, 71));
        ++expected_calls;
        check(injected_calls.load() == expected_calls);
        check(!broker.confirm_reclaimed(*key, 71));
        check(!broker.try_watch(*key, 71));
        auto replacement = broker.mint_eventfd(72);
        check(replacement && broker.revoke(*replacement, 72));
        check(broker.confirm_reclaimed(*replacement, 72));
    }
    // EBADF is NOT a valid receipt. Once the manager attempted physical
    // close, stale numeric descriptors must not be closed a second time.
    for (const int failure : {EBADF, EIO}) {
        NativeFdAuthorityBroker broker(1);
        auto token = broker.mint_eventfd(81);
        check(token && broker.revoke(*token, 81));
        arm(failure);
        check(!broker.confirm_reclaimed(*token, 81));
        ++expected_calls;
        check(injected_calls.load() == expected_calls);
        check(!broker.confirm_reclaimed(*token, 81));
        check(injected_calls.load() == expected_calls);
        check(!broker.try_watch(*token, 81));
        // One unusable authority slot must not poison unrelated host work.
        auto healthy = broker.mint_eventfd(82);
        check(healthy && broker.revoke(*healthy, 82));
        check(broker.confirm_reclaimed(*healthy, 82));
    }
    // eventfd read/write EINTR means the operation did not complete.
    // Retry the same live descriptor exactly once through the ordinary loop;
    // do not surface a false capability denial or leave a dirty counter.
    {
        using Manager = oreslang::runtime::NativeEpollReactorManager;
        Manager manager(1);
        auto token = manager.open_owned_eventfd();
        check(token.has_value());

        injected_write_errno.store(EINTR, std::memory_order_release);
        check(manager.signal_owned_eventfd(*token));
        check(injected_write_calls.load(std::memory_order_acquire) == 1);

        injected_read_errno.store(EINTR, std::memory_order_release);
        check(manager.drain_owned_eventfd(*token));
        check(injected_read_calls.load(std::memory_order_acquire) == 1);

        check(manager.close_owned_eventfd(*token));
    }

    check(injected_errno.load() == 0);
    check(injected_read_errno.load() == 0);
    check(injected_write_errno.load() == 0);
    std::printf("NATIVE_FD_EINTR_PASS close=%u read=%u write=%u\n",
                expected_calls,
                injected_read_calls.load(std::memory_order_relaxed),
                injected_write_calls.load(std::memory_order_relaxed));
    return 0;
}
