#include "oreslang/runtime/DurableLockQueues.hpp"
#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <thread>
#include <vector>

using oreslang::runtime::DurableLockQueues;

template <typename E, typename F>
static void rejects(F&& f) {
    bool threw = false;
    try { f(); } catch (const E&) { threw = true; }
    assert(threw);
}

int main() {
    DurableLockQueues a(11, {2, 4, 3, 12});
    DurableLockQueues b(12, {2, 4, 3, 12});
    const auto a1 = a.enqueue(10, 1, 3);
    const auto a2 = a.enqueue(10, 2, 3);
    const auto a3 = a.enqueue(10, 3, 3);
    const auto other = a.enqueue(20, 4, 3);
    assert(a1.runnable && !a2.runnable && !a3.runnable && other.runnable);
    assert(a.owns(a1.ticket) && a.owns(other.ticket));
    rejects<std::logic_error>([&] { a.release(a2.ticket); });
    rejects<std::logic_error>([&] { a.release(b.enqueue(10, 1, 3).ticket); });
    rejects<std::logic_error>([&] { a.enqueue(10, 3, 1); });
    rejects<std::length_error>([&] { a.enqueue(30, 5, 1); });
    rejects<std::length_error>([&] { a.enqueue(10, 5, 1); });
    auto next = a.release(a1.ticket);
    assert(next && *next == a2.ticket && a.owns(a2.ticket));
    rejects<std::logic_error>([&] { a.release(a1.ticket); });
    a.cancel_waiter(a3.ticket);
    rejects<std::logic_error>([&] { a.cancel_waiter(a2.ticket); });
    assert(!a.release(a2.ticket));
    assert(!a.release(other.ticket));
    assert(a.snapshot().entries == 0 && a.snapshot().locks == 0);
    const auto replacement = a.enqueue(10, 1, 1);
    assert(replacement.ticket.sequence != a1.ticket.sequence);
    rejects<std::logic_error>([&] { a.release(a1.ticket); });
    a.quiesce();
    rejects<std::logic_error>([&] { a.enqueue(10, 8, 1); });
    assert(!a.drained());
    a.release(replacement.ticket);
    assert(a.drained());
    rejects<std::invalid_argument>([] { DurableLockQueues bad(0); });
    rejects<std::invalid_argument>([&] { b.enqueue(0, 90, 1); });
    rejects<std::invalid_argument>([&] { b.enqueue(1, 0, 1); });
    rejects<std::invalid_argument>([&] { b.enqueue(1, 90, 0); });

    // Concurrency: different lock IDs are runnable independently. FIFO remains
    // per lock even with simultaneous producer admission.
    DurableLockQueues concurrent(77, {8, 512, 512, 4096});
    std::atomic<std::uint64_t> ids{1};
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([&] {
            for (int n = 0; n < 100; ++n) {
                concurrent.enqueue(9, ids.fetch_add(1), 1);
            }
        });
    }
    for (auto& t : threads) t.join();
    auto snap = concurrent.snapshot();
    assert(snap.entries == 400 && snap.locks == 1);
    // Dispatch FIFO by replaying next ticket from release.
    // There is no direct peek in the guest API by design; first ticket is
    // obtained from a dedicated FIFO smoke below.
    DurableLockQueues fifo(78);
    auto t1 = fifo.enqueue(9, 1, 1);
    auto t2 = fifo.enqueue(9, 2, 1);
    auto t3 = fifo.enqueue(9, 3, 1);
    assert(fifo.release(t1.ticket) == t2.ticket);
    assert(fifo.release(t2.ticket) == t3.ticket);
    assert(!fifo.release(t3.ticket));
    concurrent.quiesce();
    assert(!concurrent.drained());
    return 0;
}
