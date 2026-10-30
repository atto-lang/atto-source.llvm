#include "state_region.hpp"
#include <cassert>
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
    assert(region.acquire(Mode::read) == Status::ok);
    assert(region.acquire(Mode::read) == Status::ok);
    assert(region.acquire(Mode::write) == Status::conflicting_access);
    assert(region.retire() == Status::busy);
    assert(region.release(Mode::read) == Status::ok);
    assert(region.release(Mode::read) == Status::ok);
    assert(region.acquire(Mode::write) == Status::ok);
    assert(region.pin() == Status::ok);
    assert(region.acquire(Mode::read) == Status::conflicting_access);
    assert(region.retire() == Status::busy);
    uint64_t value = 0;
    for (int i = 0; i < 100000; ++i) ++value;
    assert(value == 100000);
    assert(region.release(Mode::write) == Status::ok);
    assert(region.version == 1);
    assert(region.retire() == Status::busy);
    assert(region.unpin() == Status::ok);
    assert(region.retire() == Status::ok);
    assert(region.acquire(Mode::read) == Status::retired);
    assert(region.release(Mode::write) == Status::bad_release);
    assert(region.unpin() == Status::bad_release);
    assert(allocations.load() == before);
    std::puts("state_region: PASS (100000 mutations; 0 counted C++ heap allocations)");
}
