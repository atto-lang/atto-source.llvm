#include "state_region.hpp"
#include <cassert>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL %s:%d: %s\\n", __FILE__, __LINE__, #x); std::abort(); } } while (false)
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <atomic>

static std::atomic<size_t> allocations{0};
void* operator new(std::size_t n) {
    ++allocations;
    if (void* p = std::malloc(n)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

using ores::state::Region;
using ores::state::Mode;
using ores::state::Status;

int main() {
    Region region;
    const size_t before = allocations.load();
    CHECK(region.acquire(Mode::read) == Status::ok);
    CHECK(region.acquire(Mode::read) == Status::ok);
    CHECK(region.acquire(Mode::write) == Status::conflicting_access);
    CHECK(region.retire() == Status::busy);
    CHECK(region.release(Mode::read) == Status::ok);
    CHECK(region.release(Mode::read) == Status::ok);
    CHECK(region.acquire(Mode::write) == Status::ok);
    CHECK(region.pin() == Status::ok);
    CHECK(region.acquire(Mode::read) == Status::conflicting_access);
    CHECK(region.retire() == Status::busy);
    uint64_t value = 0;
    for (int i = 0; i < 100000; ++i) ++value;
    CHECK(value == 100000);
    CHECK(region.release(Mode::write) == Status::ok);
    CHECK(region.version == 1);
    CHECK(region.retire() == Status::busy);
    CHECK(region.unpin() == Status::ok);
    CHECK(region.retire() == Status::ok);
    CHECK(region.acquire(Mode::read) == Status::retired);
    CHECK(region.release(Mode::write) == Status::bad_release);
    CHECK(region.unpin() == Status::bad_release);
    CHECK(allocations.load() == before);
    std::puts("state_region: PASS (100000 mutations; 0 counted C++ heap allocations)");
}
