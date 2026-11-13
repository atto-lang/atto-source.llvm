// HOST RUNTIME acceptance only. This does not prove .atto compiler lowering.
#include "oreslang/runtime/NativeFixedBufferPool.hpp"
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <optional>
#include <thread>
#include <vector>

static std::atomic<std::size_t> allocations{0};
void* operator new(std::size_t n) {
    allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n == 0 ? 1 : n)) return p;
    throw std::bad_alloc();
}
[[gnu::noinline]] static void tracked_free(void* p) noexcept { std::free(p); }
void operator delete(void* p) noexcept { tracked_free(p); }
void operator delete(void* p, std::size_t) noexcept { tracked_free(p); }
void* operator new[](std::size_t n) {
    allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n == 0 ? 1 : n)) return p;
    throw std::bad_alloc();
}
void operator delete[](void* p) noexcept { tracked_free(p); }
void operator delete[](void* p, std::size_t) noexcept { tracked_free(p); }

using oreslang::runtime::NativeFixedBufferPool;
int main() {
    for (auto dims : {std::pair{0u, 16u}, {1u, 0u}, {2u, 1024u * 1024u * 1024u}}) {
        bool rejected = false;
        try { NativeFixedBufferPool invalid(dims.first, dims.second); }
        catch (const std::invalid_argument&) { rejected = true; }
        assert(rejected);
    }
    NativeFixedBufferPool pool(2, 64);
    assert(!pool.try_acquire(0));
    auto first = pool.try_acquire(11);
    auto second = pool.try_acquire(11);
    assert(first && second && !pool.try_acquire(11));
    std::uintptr_t before = 0, after = 0;
    assert(first->with_mutable_bytes(11, [&](std::span<std::uint8_t> s) {
        before = reinterpret_cast<std::uintptr_t>(s.data());
        s[0] = 42;
    }));
    assert(!first->with_mutable_bytes(22, [](auto){}));
    assert(!pool.try_prepare_transfer(*first, 22));  // wrong sender retains original
    auto pending = pool.try_prepare_transfer(*first, 11);
    assert(pending);
    assert(!first->release()); // moved-from sender cannot release escrow
    assert(!first->with_read_only_bytes(11, [](auto){}));
    assert(!pending->commit(0)); // invalid receiver keeps escrow intact
    auto receiver = pending->commit(22);
    assert(receiver && !pending->abort());
    assert(receiver->with_read_only_bytes(22, [&](std::span<const std::uint8_t> s) {
        after = reinterpret_cast<std::uintptr_t>(s.data());
        assert(s[0] == 42);
    }));
    assert(before == after && before != 0);  // no payload copy on transfer
    auto moved = std::move(*receiver);
    assert(!receiver->release());
    assert(moved.release() && !moved.release());
    assert(second->release() && pool.in_use() == 0);
    auto clean = pool.try_acquire(44);
    assert(clean && clean->with_read_only_bytes(44, [](auto s) { assert(s[0] == 0); }));
    assert(clean->release());

    const auto warmup_allocs = allocations.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < 100000; ++i) {
        auto a = pool.try_acquire(100);
        assert(a);
        assert(a->with_mutable_bytes(100, [](auto s) { s[0] = 99; }));
        auto pending = pool.try_prepare_transfer(*a, 100);
        assert(pending && !a->release());
        auto delivered = pending->commit(200);
        assert(delivered);
        assert(delivered->with_read_only_bytes(200, [](auto s) { assert(s[0] == 99); }));
        assert(delivered->release());
    }
    assert(allocations.load(std::memory_order_relaxed) == warmup_allocs);
    assert(pool.in_use() == 0);

    // A failed mailbox offer explicitly aborts escrow and restores the
    // original owner without copying payload or consuming another slot.
    auto original = pool.try_acquire(300);
    assert(original);
    auto abortable = pool.try_prepare_transfer(*original, 300);
    assert(abortable && !original->release());
    auto restored = abortable->abort();
    assert(restored && !abortable->commit(400));
    assert(restored->with_mutable_bytes(300, [](auto s) { s[0] = 23; }));
    assert(restored->release());

    // Abandoned/cancelled pending handoff must scrub+return its slot, rather
    // than leaving an escrow stranded or exposing it to the old sender.
    auto abandoned = pool.try_acquire(301);
    assert(abandoned);
    {
        auto transfer = pool.try_prepare_transfer(*abandoned, 301);
        assert(transfer && !abandoned->release());
    }
    assert(pool.in_use() == 0);

    // Pool retirement rejects commitment but allows explicit rollback and
    // teardown, including when the prepared handoff outlives its pool.
    {
        NativeFixedBufferPool retiring(1, 8);
        auto value = retiring.try_acquire(7);
        assert(value);
        auto transfer = retiring.try_prepare_transfer(*value, 7);
        assert(transfer);
        retiring.retire();
        assert(!transfer->commit(8));
        auto rolled_back = transfer->abort();
        assert(rolled_back && rolled_back->release());
    }

    auto survivor = pool.try_acquire(5);
    assert(survivor);
    pool.retire();
    assert(!pool.try_acquire(5));
    assert(!survivor->with_read_only_bytes(5, [](auto) {}));
    assert(survivor->release());
    assert(pool.in_use() == 0);

    std::optional<NativeFixedBufferPool::Lease> detached;
    {
        NativeFixedBufferPool ephemeral(1, 8);
        detached = ephemeral.try_acquire(7);
        assert(detached);
    }
    assert(!detached->with_read_only_bytes(7, [](auto){}));
    assert(detached->release());  // retains storage through pool destruction

    NativeFixedBufferPool concurrent(8, 128);
    std::atomic<int> completed{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&concurrent, &completed, t] {
            for (int i = 0; i < 4000; ++i) {
                auto lease = concurrent.try_acquire(static_cast<std::uint64_t>(t + 1));
                if (!lease) { std::this_thread::yield(); continue; }
                assert(lease->with_mutable_bytes(t + 1, [](auto s) { s[0] = 1; }));
                assert(lease->release());
            }
            completed.fetch_add(1);
        });
    }
    for (auto& t : threads) t.join();
    assert(completed == 8 && concurrent.in_use() == 0);
}
