#include "oreslang/runtime/FixedBufferReusePrototype.hpp"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <new>
#include <thread>

static std::atomic<std::size_t> allocations{0};
void* operator new(std::size_t n) {
    allocations.fetch_add(1, std::memory_order_relaxed);
    if (auto* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
static void check(bool b) { if (!b) std::abort(); }
using Pool = oreslang::runtime::FixedBufferReusePrototype<4, 64>;
template<std::size_t N> static void ring_regression() {
    using P = oreslang::runtime::FixedBufferReusePrototype<N, 8>;
    P p;
    std::array<std::optional<typename P::Lease>, N> leases{};
    const auto before = allocations.load();
    for (unsigned round = 0; round < 1000; ++round) {
        for (std::size_t i = 0; i < N; ++i) {
            leases[i] = p.try_acquire();
            check(leases[i].has_value());
            for (std::size_t j = 0; j < i; ++j)
                check(p.backing_slot(*leases[i]) != p.backing_slot(*leases[j]));
            for (std::size_t k = 0; k < 8; ++k)
                check(p.read(*leases[i], k) == 0 && p.write(*leases[i], k, 255));
        }
        check(!p.try_acquire());
        for (std::size_t i = N; i > 0; --i) {
            const auto old = *leases[i-1];
            check(p.release(old) && !p.release(old) && !p.read(old, 0));
        }
        check(p.outstanding() == 0);
    }
    check(allocations.load() == before);
}
int main() {
    ring_regression<1>(); ring_regression<3>(); ring_regression<7>(); ring_regression<64>();
    Pool pool, foreign;
    const auto before = allocations.load();
    for (int i = 0; i < 250000; ++i) {
        auto a = pool.try_acquire(), b = pool.try_acquire(), c = pool.try_acquire(), d = pool.try_acquire();
        check(a && b && c && d && !pool.try_acquire());
        std::array<Pool::Lease, 4> leases{*a,*b,*c,*d};
        for (std::size_t k = 0; k < leases.size(); ++k) {
            const auto& h = leases[k];
            check(pool.backing_slot(h) == k && !foreign.release(h));
            for (std::size_t j = 0; j < 64; ++j) check(pool.read(h,j) == 0);
            for (std::size_t j = 0; j < 64; ++j) check(pool.write(h, j, static_cast<std::uint8_t>(j + 1)));
            check(!pool.write(h,64,1) && !pool.read(h,64));
        }
        for (const auto& h : leases) {
            check(pool.release(h) && !pool.release(h) && !pool.read(h,0));
        }
        check(pool.outstanding() == 0);
    }
    const auto after = allocations.load();
    check(before == after && Pool::backing_bytes() == 256);
    auto old = pool.try_acquire(); check(old && pool.release(*old));
    auto fresh = pool.try_acquire(); check(fresh && !pool.write(*old,0,1));
    check(pool.release(*fresh));
    // Thread startup allocations are outside the measured window.
    std::atomic<unsigned> ready{0}, done{0};
    std::atomic<bool> start{false};
    std::array<std::thread, 4> threads;
    for (auto& t : threads) t = std::thread([&] {
        ready.fetch_add(1);
        while (!start.load()) std::this_thread::yield();
        for (int i = 0; i < 10000; ++i) {
            auto h = pool.try_acquire();
            if (!h) { --i; std::this_thread::yield(); continue; }
            for (std::size_t j = 0; j < 64; ++j) check(pool.read(*h,j) == 0 && pool.write(*h,j,7));
            check(pool.release(*h));
        }
        done.fetch_add(1);
    });
    while (ready.load() != threads.size()) std::this_thread::yield();
    const auto concurrent_before = allocations.load();
    start.store(true);
    while (done.load() != threads.size()) std::this_thread::yield();
    const auto concurrent_after = allocations.load();
    for (auto& t : threads) t.join();
    check(concurrent_before == concurrent_after);
    check(pool.outstanding() == 0);
    auto last = pool.try_acquire(); check(last.has_value());
    pool.close_admission(); check(!pool.try_acquire() && pool.release(*last));
    check(pool.outstanding() == 0);
    std::cout << "HOST_ONLY completed_cycles=1000000 global_new_delta=" << after-before
              << " concurrent_cycles=40000 concurrent_new_delta=" << concurrent_after-concurrent_before
              << " retained_payload_bytes=" << Pool::backing_bytes() << '\n';
}
