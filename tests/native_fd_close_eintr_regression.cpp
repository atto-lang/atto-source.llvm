// Linux-only exact-source regression: close(2) may report EINTR AFTER the
// descriptor has been released. Never retry the numeric FD: it may have
// been reassigned to another thread. The trusted host ledger must not leak
// a slot on EINTR, and must fail closed on EBADF/unexpected close failures.
// Requires -Wl,--wrap=close; the wrapper executes the REAL kernel close
// first, then alters its userspace result for deterministic fault injection.
#include "oreslang/runtime/NativeFdAuthorityBroker.hpp"
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

static std::atomic<int> injected_errno{0};
static std::atomic<unsigned> injected_calls{0};
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
    check(injected_errno.load() == 0);
    std::printf("NATIVE_FD_CLOSE_EINTR_PASS injected=%u\n", expected_calls);
    return 0;
}
