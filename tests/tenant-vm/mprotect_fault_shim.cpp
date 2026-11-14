// Linker-level fault injection for actual TenantVm.cpp mprotect call sites.
// No production build depends on this file. Linux/GNU ld --wrap is test-only.
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <sys/mman.h>
#include <signal.h>
#include <pthread.h>

namespace {
std::atomic<bool> fail_install{false};
std::atomic<bool> fail_restore{false};
std::atomic<int> fail_carrier_allocations{0};
std::atomic<bool> fail_signal_stack_registration{false};
std::atomic<int> recovered_sigtrap_was_blocked{-1};
}

// Observe only the test-linked original carrier *after* siglongjmp. The test
// deliberately blocks SIGTRAP before creating the worker. -1 means no recovery,
// -2 means pthread_sigmask failed, 0 indicates an incorrectly unblocked SIGTRAP.
extern "C" void tenant_vm_test_reset_recovery_mask() {
    recovered_sigtrap_was_blocked.store(-1, std::memory_order_release);
}
extern "C" int tenant_vm_test_recovery_mask_status() {
    return recovered_sigtrap_was_blocked.load(std::memory_order_acquire);
}
extern "C" void tenant_vm_test_record_recovery_mask() {
    sigset_t current{};
    const int status = pthread_sigmask(SIG_SETMASK, nullptr, &current);
    const int blocked = status == 0 ? sigismember(&current, SIGTRAP) : -2;
    recovered_sigtrap_was_blocked.store(blocked, std::memory_order_release);
}

extern "C" void tenant_vm_test_fail_next_guard_install() {
    fail_install.store(true, std::memory_order_release);
}
extern "C" void tenant_vm_test_fail_next_guard_restore() {
    fail_restore.store(true, std::memory_order_release);
}
extern "C" void tenant_vm_test_fail_next_signal_stack_registration() {
    fail_signal_stack_registration.store(true, std::memory_order_release);
}
// Deliberately intercept only TenantVm.cpp's own carrier setup. A link-wide
// --wrap=sigaltstack also hits libasan's startup hook and aborts the process.
extern "C" int tenant_vm_test_carrier_sigaltstack(const stack_t* ss, stack_t* old_ss) {
    if (ss && !(ss->ss_flags & SS_DISABLE) &&
        fail_signal_stack_registration.exchange(false, std::memory_order_acq_rel)) {
        errno = ENOMEM;
        return -1;
    }
    return sigaltstack(ss, old_ss);
}
extern "C" void tenant_vm_test_fail_next_carrier_allocations(int count) {
    fail_carrier_allocations.store(count, std::memory_order_release);
}
extern "C" void* __real_mmap(void*, std::size_t, int, int, int, off_t);
extern "C" void* __wrap_mmap(void* addr, std::size_t length, int protection,
                               int flags, int fd, off_t offset) {
    // Only the host's ordinary 8 MiB carrier stack (+ 1 MiB guard), not the VM cage,
    // signal stack, or malloc/ASan internal mappings. The wrapper is linked for tests only.
    constexpr std::size_t stack_map_size = (std::size_t{9} << 20);
    if (length == stack_map_size && protection == (PROT_READ | PROT_WRITE)) {
        int remaining = fail_carrier_allocations.load(std::memory_order_acquire);
        while (remaining > 0) {
            if (fail_carrier_allocations.compare_exchange_weak(remaining, remaining - 1,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                errno = ENOMEM;
                return MAP_FAILED;
            }
        }
    }
    return __real_mmap(addr, length, protection, flags, fd, offset);
}
extern "C" int __real_mprotect(void*, std::size_t, int);
extern "C" int __wrap_mprotect(void* addr, std::size_t bytes, int protection) {
    constexpr std::size_t guard_bytes = std::size_t{1} << 20;
    if (bytes == guard_bytes && protection == PROT_NONE &&
        fail_install.exchange(false, std::memory_order_acq_rel)) {
        errno = ENOMEM;
        return -1;
    }
    if (bytes <= guard_bytes && protection == (PROT_READ | PROT_WRITE) &&
        fail_restore.exchange(false, std::memory_order_acq_rel)) {
        errno = ENOMEM;
        return -1;
    }
    return __real_mprotect(addr, bytes, protection);
}
