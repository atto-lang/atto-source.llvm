// Linker-level fault injection for actual TenantVm.cpp mprotect call sites.
// No production build depends on this file. Linux/GNU ld --wrap is test-only.
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <sys/mman.h>

namespace {
std::atomic<bool> fail_install{false};
std::atomic<bool> fail_restore{false};
std::atomic<int> fail_carrier_allocations{0};
}

extern "C" void tenant_vm_test_fail_next_guard_install() {
    fail_install.store(true, std::memory_order_release);
}
extern "C" void tenant_vm_test_fail_next_guard_restore() {
    fail_restore.store(true, std::memory_order_release);
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
