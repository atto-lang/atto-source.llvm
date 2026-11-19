#include "oreslang/runtime/ActorSlabHeap32.hpp"
#include <cassert>
#include <cstdlib>
#include <new>
#include <cstdint>
#include <iostream>
#include <limits>
#include <vector>
using oreslang::runtime::vm::ActorSlabHeap32;
static std::size_t new_calls = 0;
void* operator new(std::size_t n) { ++new_calls; if (auto* p = std::malloc(n)) return p; throw std::bad_alloc(); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
static int checks = 0;
void yes(bool v) { ++checks; if (!v) { std::cerr << "FAIL CHECK " << checks << "\n"; std::abort(); } }
int main() {
    {
        std::vector<std::uint8_t> memory(100000, 0);
        ActorSlabHeap32 a(9000); a.bind(memory.data(), 4011);
        a.bind(memory.data(), 4011); // exact repeat is safe and idempotent
        bool wrong_base_rejected = false, foreign_cage_rejected = false;
        try { a.bind(memory.data(), 8192); }
        catch (const std::invalid_argument&) { wrong_base_rejected = true; }
        try { a.bind(memory.data() + 1, 4011); }
        catch (const std::invalid_argument&) { foreign_cage_rejected = true; }
        yes(wrong_base_rejected);
        yes(foreign_cage_rejected);
        yes(sizeof(ActorSlabHeap32::Offset32) == 4);
        yes(a.metadata_bytes() <= 64 * a.page_count());
        auto x = a.allocate(8, 8), y = a.allocate(16, 16);
        yes(x && y && x != y && x % 16 == 0 && y % 16 == 0);
        memory[x] = 91;
        yes(!a.release(x, 32, 16));
        yes(!a.release(x + 1, 8, 8));
        yes(a.release(x, 8, 8) && memory[x] == 0);
        yes(!a.release(x, 8, 8));
        auto z = a.allocate(8, 8);
        yes(z == x);
        yes(a.release(z, 8, 8) && a.release(y, 16, 16));
        auto large = a.allocate(7200, 32);
        yes(large != 0 && large % 32 == 0);
        memory[large + 7199] = 42;
        yes(!a.release(large, 7200, 64));
        yes(a.release(large, 7200, 32) && memory[large + 7199] == 0);
    }
    {
        std::vector<std::uint8_t> memory(65536, 0);
        ActorSlabHeap32 a(16384); a.bind(memory.data(), 4096);
        yes(a.allocate(0, 16) == 0);
        yes(a.allocate(4, 3) == 0);
        yes(a.allocate(1, 8192) == 0);
        yes(a.allocate(UINT32_MAX, 16) == 0);
        yes(a.allocate(UINT32_MAX-7, 16) == 0);
        std::vector<std::uint32_t> slots;
        while (auto x = a.allocate(64, 16)) slots.push_back(x);
        yes(slots.size() == 256);
        yes(a.allocate(64, 16) == 0);
        for (auto x : slots) yes(a.release(x, 64, 16));
        yes(a.allocate(16384, 16) != 0);
    }
    {
        std::vector<std::uint8_t> memory(65536, 0);
        ActorSlabHeap32 a(200); a.bind(memory.data(), 17);
        auto x = a.allocate(24, 32);
        yes(x && x % 32 == 0);
        yes(a.release(x, 24, 32));
        auto y = a.allocate(32, 32);
        yes(y == x);
    }
    {
        std::vector<std::uint8_t> memory(65536, 0);
        ActorSlabHeap32 a(4096); a.bind(memory.data(), 4096);
        auto x = a.allocate(4096, 4096);
        yes(x == 4096);
        yes(a.release(x, 4096, 4096));
    }
    {
        std::vector<std::uint8_t> memory(8192, 0);
        ActorSlabHeap32 a(128); a.bind(memory.data(), 4096);
        auto x = a.allocate(128, 16);
        yes(x != 0 && a.allocate(128, 16) == 0);
        yes(a.release(x, 128, 16));
        a.reset();
        yes(a.allocate(128, 16) == x);
    }
    {
        std::vector<std::uint8_t> memory(8192, 0);
        ActorSlabHeap32 heap(4096); heap.bind(memory.data(), 4096);
        yes(heap.allocate(100, 16) != 0);
        yes(heap.allocate(100, 16) != 0);
        yes(heap.allocate(64, 16) != 0); // distinct small size classes coexist
    }
    {
        std::vector<std::uint8_t> memory(8192, 0);
        ActorSlabHeap32 heap(4096); heap.bind(memory.data(), 4096);
        const auto before = new_calls;
        for (std::size_t n = 0; n < 1000; ++n) {
            const auto ptr = heap.allocate(32, 16);
            yes(ptr != 0 && heap.release(ptr, 32, 16));
        }
        yes(before == new_calls);
    }
    std::cout << "COMPACT_ACTOR_SLAB_TEST_PASS checks=" << checks << "\n";
}
