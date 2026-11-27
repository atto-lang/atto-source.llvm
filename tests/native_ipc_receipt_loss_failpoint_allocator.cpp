// Allocation-failure injection is deliberately isolated in a separate TU.
// The replacement new/delete pair uses matching malloc/free; keeping it out
// of the tested call sites also avoids GCC's inlined mismatched-new-delete
// false positive without disabling warnings or the allocation failure test.
#include <atomic>
#include <cstdlib>
#include <new>

static std::atomic<int> fails_after{-1};
void set_ipc_test_allocation_failure(int count) noexcept { fails_after.store(count); }
void* operator new(std::size_t n) {
    const int prev = fails_after.load();
    if (prev >= 0 && fails_after.fetch_sub(1) == 0) throw std::bad_alloc();
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
