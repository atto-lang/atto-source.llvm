#include "oreslang/runtime/TrustedActorBufferPool.hpp"

#include <atomic>
#include <cassert>
#include <cstdlib>
#include <cstdint>
#include <new>
#include <stdexcept>
#include <thread>
#include <utility>

using oreslang::runtime::TrustedActorBufferPool;
using oreslang::runtime::TrustedBufferActor;

// Instrument C++ global allocation on the metadata-only ownership path.
// This is NOT a whole-program malloc/syscall/GC measurement.
static std::atomic<std::size_t> allocations{0};
void* operator new(std::size_t bytes) {
    allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(bytes ? bytes : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

template <class F> bool rejects(F&& fn) {
    try { fn(); } catch (const std::exception&) { return true; }
    return false;
}

int main() {
    const TrustedBufferActor a{1, 1}, b{2, 1}, b_new{2, 2};
    assert(rejects([] { TrustedActorBufferPool bad(0, 128, 4); }));
    assert(rejects([] { TrustedActorBufferPool bad(1, 0, 4); }));
    assert(rejects([] { TrustedActorBufferPool bad(1, 128, 0); }));

    TrustedActorBufferPool pool(11, 128, 2);
    TrustedActorBufferPool other_tenant(12, 128, 2);
    auto original = pool.allocate(a, 96);
    assert(pool.used_bytes() == 96);
    pool.write(original, a, 0, 37);
    pool.write(original, a, 95, 99);
    assert(pool.read(original, a, 0) == 37);
    assert(pool.read(original, a, 95) == 99);
    assert(rejects([&] { pool.read(original, b, 0); }));
    assert(rejects([&] { other_tenant.read(original, a, 0); }));
    assert(rejects([&] { pool.allocate(a, 33); }));
    assert(rejects([&] { pool.allocate({0, 1}, 1); }));
    assert(rejects([&] { pool.read(original, a, 96); }));

    auto loan1 = pool.borrow_read(original, a, b);
    auto loan2 = pool.borrow_read(original, a, b_new);
    assert(loan1.read(0) == 37);
    assert(loan2.read(95) == 99);
    assert(rejects([&] { pool.write(original, a, 0, 99); }));
    assert(rejects([&] { pool.take(original, a, b); }));
    assert(rejects([&] { pool.drop(original, a); }));
    assert(rejects([&] { loan1.read(96); }));
    std::atomic<bool> go{false};
    std::thread reader([lease = std::move(loan2), &go]() mutable {
        while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
        assert(lease.read(0) == 37);
        lease.release();
        assert(rejects([&] { lease.read(0); }));
    });
    go.store(true, std::memory_order_release);
    reader.join();
    loan1.release();

    // The 50k ownership changes preserve payload and require zero C++ global
    // new calls on the *move* path itself, after the initial allocation.
    auto current = original;
    auto who = a;
    auto next = b;
    const auto before = allocations.load(std::memory_order_relaxed);
    for (int i = 0; i < 50000; ++i) {
        const auto stale = current;
        current = pool.take(current, who, next);
        std::swap(who, next);
        if (i == 0) {
            // stale sender alias rejected even if caller claims new ownership
            assert(rejects([&] { pool.read(stale, who, 0); }));
        }
    }
    const auto after = allocations.load(std::memory_order_relaxed);
    // The first iteration intentionally exercises an exception (and may
    // allocate for its message), so compare a second pure-move sequence below.
    assert(after >= before);
    const auto pure_before = allocations.load(std::memory_order_relaxed);
    for (int i = 0; i < 50000; ++i) {
        current = pool.take(current, who, next);
        std::swap(who, next);
    }
    assert(allocations.load(std::memory_order_relaxed) == pure_before);
    assert(pool.read(current, who, 0) == 37);
    assert(pool.read(current, who, 95) == 99);
    assert(rejects([&] { pool.take(original, a, b); }));
    assert(rejects([&] { pool.read(current, b_new, 0); }));

    pool.drop(current, who);
    assert(pool.used_bytes() == 0);
    assert(rejects([&] { pool.read(current, who, 0); }));
    auto replacement = pool.allocate(a, 128);
    assert(rejects([&] { pool.allocate(a, 1); }));
    auto last_loan = pool.borrow_read(replacement, a, b);
    pool.retire(); // revoke without freeing loan backing storage early
    assert(pool.used_bytes() == 0);
    assert(rejects([&] { last_loan.read(0); }));
    assert(rejects([&] { pool.allocate(a, 1); }));
    assert(rejects([&] { pool.take(replacement, a, b); }));
    last_loan.release();
    pool.retire(); // idempotent
    other_tenant.retire();

    // Destruction before lease release is safe because the lease owns a
    // shared, now-revoked backing state rather than a dangling pool pointer.
    auto transient = std::make_unique<TrustedActorBufferPool>(13, 16, 1);
    auto item = transient->allocate(a, 8);
    auto detached_loan = transient->borrow_read(item, a, b);
    transient.reset();
    assert(rejects([&] { detached_loan.read(0); }));
    detached_loan.release();
}
