#include "oreslang/runtime/TrustedActorBufferMailbox.hpp"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>

using oreslang::runtime::TrustedActorBufferMailbox;
using oreslang::runtime::TrustedActorBufferPool;
using oreslang::runtime::TrustedBufferActor;

template<class F> bool rejects(F&& f) {
    try { f(); } catch (const std::exception&) { return true; }
    return false;
}

int main() {
    const TrustedBufferActor a{1,1}, b{2,1}, b_reused{2,2};
    auto pool = std::make_shared<TrustedActorBufferPool>(100, 1024, 16);
    assert(rejects([&] { TrustedActorBufferMailbox bad(pool, b, 0); }));
    assert(rejects([&] { TrustedActorBufferMailbox bad(nullptr, b, 1); }));
    assert(rejects([&] { TrustedActorBufferMailbox bad(pool, {0,1}, 1); }));

    TrustedActorBufferMailbox forward(pool, b, 1);
    TrustedActorBufferMailbox reverse(pool, a, 1);
    auto first = pool->allocate(a, 128);
    auto second = pool->allocate(a, 64);
    pool->write(first, a, 0, 19);
    pool->write(second, a, 0, 27);

    forward.send(first, a);
    assert(forward.pending() == 1);
    assert(rejects([&] { pool->read(first, a, 0); }));
    assert(rejects([&] { forward.send(second, a); })); // backpressure
    assert(pool->read(second, a, 0) == 27); // failure didn't take
    assert(rejects([&] { forward.receive(b_reused); })); // incarnation
    assert(forward.pending() == 1);
    auto received = forward.receive(b);
    assert(pool->read(received, b, 0) == 19);
    assert(rejects([&] { forward.receive(b); }));
    assert(rejects([&] { pool->read(first, b, 0); })); // old sender handle
    reverse.send(received, b);
    auto back = reverse.receive(a);
    assert(pool->read(back, a, 0) == 19);

    // Recycle the same physical buffer through two one-slot queues.
    // The payload stays correct and memory accounting does not grow.
    const auto before_bytes = pool->used_bytes();
    for (int i = 0; i < 10000; ++i) {
        forward.send(back, a);
        received = forward.receive(b);
        reverse.send(received, b);
        back = reverse.receive(a);
    }
    assert(pool->read(back, a, 0) == 19);
    assert(pool->used_bytes() == before_bytes);

    // A borrowed buffer cannot be admitted or transferred.
    auto borrow = pool->borrow_read(back, a, b);
    assert(rejects([&] { forward.send(back, a); }));
    assert(pool->read(back, a, 0) == 19);
    borrow.release();

    // A live borrow of another actor's owned buffer does not block an
    // unrelated inbox. A delivered owner buffer also must not pin the
    // mailbox: the actor retirement gate independently fences that owner.
    TrustedActorBufferMailbox borrowed_queue(pool, b, 1);
    auto pinned = pool->allocate(a, 5);
    borrowed_queue.send(pinned, a);
    auto pinned_reader = pool->borrow_read(back, a, b);
    assert(borrowed_queue.close()); // unrelated owner loan cannot block close
    // For a close veto, the recipient must own a live borrowed buffer.
    auto to_b = pool->allocate(a, 1);
    TrustedActorBufferMailbox to_b_queue(pool, b, 1);
    to_b_queue.send(to_b, a);
    auto owned_by_b = to_b_queue.receive(b);
    auto own_loan = pool->borrow_read(owned_by_b, b, a);
    assert(to_b_queue.close()); // delivered payload no longer belongs to queue
    assert(to_b_queue.pending() == 0);
    assert(pool->used_bytes() == 193);
    own_loan.release();
    assert(to_b_queue.close());
    pool->drop(owned_by_b, b);
    pinned_reader.release();
    assert(borrowed_queue.pending() == 0);
    assert(pool->used_bytes() == 192);

    // Closing a mailbox drops queued receiver-owned payloads; closed
    // admission must never invalidate a sender's still-owned buffer.
    forward.send(second, a);
    assert(pool->used_bytes() == 192);
    forward.close();
    assert(forward.pending() == 0);
    assert(pool->used_bytes() == 128);
    assert(rejects([&] { forward.send(back, a); }));
    assert(rejects([&] { forward.receive(b); }));
    assert(pool->read(back, a, 0) == 19);

    // Close/send race: either send is denied (sender keeps ownership),
    // or accepted and then terminal close drops the queued receiver slot.
    for (int attempt = 0; attempt < 64; ++attempt) {
        TrustedActorBufferMailbox racing(pool, b, 1);
        auto item = pool->allocate(a, 8);
        std::atomic<bool> go{false};
        std::atomic<bool> sent{false};
        std::thread producer([&] {
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            try { racing.send(item, a); sent.store(true, std::memory_order_release); }
            catch (const std::logic_error&) { }
        });
        std::thread closer([&] {
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            racing.close();
        });
        go.store(true, std::memory_order_release);
        producer.join();closer.join();
        if (!sent.load(std::memory_order_acquire)) {
            assert(pool->read(item, a, 0) == 0);
            pool->drop(item, a);
        } else {
            assert(rejects([&] { pool->read(item, a, 0); }));
        }
    }
    assert(pool->used_bytes() == 128);
    pool->drop(back, a);
    assert(pool->used_bytes() == 0);
    reverse.close();
    pool->retire();
}
