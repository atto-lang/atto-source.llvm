// Native HOST data-lane proof only. Not a compiled AttoLang guest test.
#include "oreslang/runtime/NativeFixedBufferMailbox.hpp"
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
#include <optional>
#include <thread>

using oreslang::runtime::NativeFixedBufferPool;
using oreslang::runtime::BufferActorIdentity;
static constexpr BufferActorIdentity K(std::uint64_t actor,
                                       std::uint64_t generation = 1,
                                       std::uint64_t tenant = 1) noexcept {
    return {tenant, actor, generation};
}

using oreslang::runtime::NativeFixedBufferMailbox;
static std::atomic<std::size_t> allocations{0};
void* operator new(std::size_t size) {
    allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* ptr = std::malloc(size ? size : 1)) return ptr;
    throw std::bad_alloc();
}
void operator delete(void* ptr) noexcept { std::free(ptr); }
void operator delete(void* ptr, std::size_t) noexcept { std::free(ptr); }
void* operator new[](std::size_t size) {
    allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* ptr = std::malloc(size ? size : 1)) return ptr;
    throw std::bad_alloc();
}
void operator delete[](void* ptr) noexcept { std::free(ptr); }
void operator delete[](void* ptr, std::size_t) noexcept { std::free(ptr); }

int main() {
    for (const auto count : {0u, (1u << 20) + 1}) {
        bool rejected = false;
        try { NativeFixedBufferMailbox bad(count, K(2)); }
        catch (const std::invalid_argument&) { rejected = true; }
        assert(rejected);
    }
    NativeFixedBufferPool pool(3, 64);
    NativeFixedBufferMailbox mailbox(1, K(22));
    auto first = pool.try_acquire(K(11));
    auto second = pool.try_acquire(K(11));
    assert(first && second);
    std::uintptr_t before = 0, after = 0;
    assert(first->with_mutable_bytes(K(11), [&](auto s) {
        before = reinterpret_cast<std::uintptr_t>(s.data());
        s[0] = 73;
    }));
    assert(!mailbox.try_offer(pool, *first, K(99))); // wrong actor
    assert(!mailbox.try_offer(pool, *first, K(11, 2))); // stale generation
    assert(!mailbox.try_offer(pool, *first, K(11, 1, 2))); // wrong tenant // rejected without touching sender
    assert(first->with_read_only_bytes(K(11), [](auto s) { assert(s[0] == 73); }));
    assert(!first->with_read_only_bytes(K(11, 2), [](auto) {}));
    assert(mailbox.try_offer(pool, *first, K(11)));
    assert(!first->release()); // committed sender is no longer an owner
    assert(mailbox.charged() == 1 && mailbox.queued() == 1);
    assert(!mailbox.try_offer(pool, *second, K(11))); // one unacknowledged credit
    assert(second->with_read_only_bytes(K(11), [](auto) {})); // rejected sender intact
    auto delivery = mailbox.try_receive();
    assert(delivery && mailbox.charged() == 1 && mailbox.queued() == 0);
    assert(!mailbox.try_offer(pool, *second, K(11))); // dequeue does not refund credit
    assert(!delivery->lease().with_read_only_bytes(K(11), [](auto){}));
    assert(delivery->lease().with_read_only_bytes(K(22), [&](auto s) {
        after = reinterpret_cast<std::uintptr_t>(s.data());
        assert(s[0] == 73);
    }));
    assert(before != 0 && before == after); // exact same native buffer backing
    assert(delivery->acknowledge_released());
    assert(!delivery->acknowledge_released());
    bool rejected_stale_delivery = false;
    try { (void)delivery->lease(); }
    catch (const std::logic_error&) { rejected_stale_delivery = true; }
    assert(rejected_stale_delivery);
    assert(mailbox.charged() == 0);
    assert(mailbox.try_offer(pool, *second, K(11)));
    auto second_delivery = mailbox.try_receive();
    assert(second_delivery && second_delivery->acknowledge_released());
    assert(pool.in_use() == 0);
    auto scrubbed = pool.try_acquire(K(55));
    assert(scrubbed && scrubbed->with_read_only_bytes(K(55), [](auto s) { assert(s[0] == 0); }));
    assert(scrubbed->release());

    NativeFixedBufferMailbox hot(2, K(88));
    const auto start_allocs = allocations.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < 100000; ++i) {
        auto source = pool.try_acquire(K(77));
        assert(source);
        assert(hot.try_offer(pool, *source, K(77)));
        assert(!source->release());
        auto item = hot.try_receive();
        assert(item);
        assert(item->acknowledge_released());
    }
    assert(allocations.load(std::memory_order_relaxed) == start_allocs);
    assert(pool.in_use() == 0 && hot.charged() == 0);

    // A dropped delivery is NOT an ACK: credit remains charged until shutdown.
    NativeFixedBufferMailbox fail_closed(1, K(9));
    auto leaked_credit = pool.try_acquire(K(8));
    assert(leaked_credit && fail_closed.try_offer(pool, *leaked_credit, K(8)));
    { auto abandoned = fail_closed.try_receive(); assert(abandoned); }
    assert(fail_closed.charged() == 1 && !fail_closed.retire());
    assert(!fail_closed.try_offer(pool, *leaked_credit, K(8)));

    // Destroying mailbox cannot free a checked-out lease's backing region.
    std::optional<NativeFixedBufferMailbox::Delivery> retained;
    {
        NativeFixedBufferMailbox ephemeral(1, K(6));
        auto src = pool.try_acquire(K(5));
        assert(src && ephemeral.try_offer(pool, *src, K(5)));
        auto taken = ephemeral.try_receive();
        assert(taken);
        retained.emplace(std::move(*taken));
        assert(retained && !ephemeral.retire());
    }
    assert(retained->lease().with_read_only_bytes(K(6), [](auto){}));
    assert(retained->acknowledge_released());
    retained.reset();

    // Retired queue cannot admit, but already accepted deliveries can settle.
    NativeFixedBufferMailbox retiring(1, K(10));
    auto src = pool.try_acquire(K(7));
    assert(src && retiring.try_offer(pool, *src, K(7)));
    assert(!retiring.retire());
    auto late = pool.try_acquire(K(7));
    assert(late && !retiring.try_offer(pool, *late, K(7)));
    auto pending = retiring.try_receive();
    assert(pending && pending->acknowledge_released() && retiring.retire());
    assert(late->release());

    // Two concurrent producers, one consumer: all mailbox/data-lane
    // transfers are bounded, fully acknowledged, and tenant/generation fenced.
    NativeFixedBufferPool parallel_pool(16, 32);
    NativeFixedBufferMailbox parallel_mailbox(8, K(200));
    std::atomic<int> acknowledged{0};
    auto producer = [&](int id) {
        for (int i = 0; i < 3000; ++i) {
            std::optional<NativeFixedBufferPool::Lease> lease;
            while (!(lease = parallel_pool.try_acquire(K(static_cast<std::uint64_t>(id)))))
                std::this_thread::yield();
            assert(lease->with_mutable_bytes(K(static_cast<std::uint64_t>(id)),
                [id](auto bytes) { bytes[0] = static_cast<std::uint8_t>(id); }));
            while (!parallel_mailbox.try_offer(parallel_pool, *lease,
                                               K(static_cast<std::uint64_t>(id))))
                std::this_thread::yield();
            assert(!lease->release());
        }
    };
    std::thread p1(producer, 101), p2(producer, 102);
    std::thread consumer([&] {
        while (acknowledged.load() < 6000) {
            auto delivery = parallel_mailbox.try_receive();
            if (!delivery) { std::this_thread::yield(); continue; }
            assert(delivery->lease().with_read_only_bytes(K(200), [](auto bytes) {
                assert(bytes[0] == 101 || bytes[0] == 102);
            }));
            assert(delivery->acknowledge_released());
            acknowledged.fetch_add(1);
        }
    });
    p1.join(); p2.join(); consumer.join();
    assert(acknowledged == 6000 && parallel_pool.in_use() == 0);
    assert(parallel_mailbox.charged() == 0 && parallel_mailbox.retire());
}
