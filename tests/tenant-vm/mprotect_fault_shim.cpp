// Linker-level fault injection for actual TenantVm.cpp mprotect call sites.
// No production build depends on this file. Linux/GNU ld --wrap is test-only.
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <sys/mman.h>

namespace {
std::atomic<bool> fail_install{false};
std::atomic<bool> fail_restore{false};
}

extern "C" void tenant_vm_test_fail_next_guard_install() {
    fail_install.store(true, std::memory_order_release);
}
extern "C" void tenant_vm_test_fail_next_guard_restore() {
    fail_restore.store(true, std::memory_order_release);
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
