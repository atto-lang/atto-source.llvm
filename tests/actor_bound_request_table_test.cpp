#include "oreslang/runtime/ActorBoundRequestTable.hpp"
#include <atomic>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string_view>
#include <thread>
#include <utility>

static std::atomic<std::size_t> news{0};
__attribute__((noinline)) void* operator new(std::size_t n) {
    news.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
__attribute__((noinline)) void* operator new[](std::size_t n) {
    news.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
__attribute__((noinline)) void operator delete(void* p) noexcept { std::free(p); }
__attribute__((noinline)) void operator delete[](void* p) noexcept { std::free(p); }
__attribute__((noinline)) void operator delete(void* p, std::size_t) noexcept { std::free(p); }
__attribute__((noinline)) void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

using Broker = oreslang::runtime::ActorBoundRequestTable<3, 2, 128>;
using Identity = Broker::ActorIncarnation;
int main() {
    Broker broker;
    constexpr Identity supervisor{1,1}, worker{2,1}, impostor{2,0};
    constexpr Identity restarted{2,2}, nonexistent{3,1}, bad{0,1};
    assert(!broker.activate(bad));
    assert(broker.activate(supervisor));
    assert(broker.activate(worker));
    assert(!broker.activate(worker)); // no duplicate incarnation
    assert(!broker.activate(restarted)); // old incarnation must retire first
    assert(!broker.begin(impostor) && !broker.begin(nonexistent));
    auto ingress = broker.begin(supervisor);
    assert(ingress && ingress->capacity() >= 9);
    std::memcpy(ingress->data(), "GET /test", 9);
    auto source_handle = ingress->publish(9);
    assert(source_handle.value);
    assert(!broker.borrow(impostor, source_handle));
    assert(!broker.borrow(worker, source_handle));
    auto moved = broker.transfer(supervisor, worker, source_handle);
    assert(moved);
    assert(!broker.borrow(supervisor, source_handle));
    assert(!broker.transfer(supervisor, worker, source_handle));
    auto lease = broker.borrow(worker, *moved);
    assert(lease && lease->view() == "GET /test");
    assert(!broker.transfer(worker, supervisor, *moved)); // pinned view
    auto retired = broker.retire(worker);
    assert(retired && *retired == 1);
    assert(!broker.borrow(worker, *moved));
    assert(!broker.close(worker, *moved));
    assert(!broker.retire(worker));
    assert(broker.available() == 1); // read lease delays reuse
    // Even after retirement, the exact old identity and lower generations
    // must never be readmitted (otherwise stale caller identity resurrects).
    assert(!broker.activate(worker));
    assert(!broker.activate(Identity{2,0}));
    assert(broker.activate(restarted));
    assert(!broker.borrow(restarted, *moved));
    assert(!broker.close(restarted, *moved));
    assert(lease->view() == "GET /test"); // retained lease still valid
    lease.reset();
    assert(broker.available() == 2);
    // Retirement invalidates in-progress writers without recycling their
    // storage until their own ingress RAII finally releases it.
    auto unfinished = broker.begin(restarted);
    assert(unfinished);
    std::memcpy(unfinished->data(), "abc", 3);
    auto closed = broker.retire(restarted);
    assert(closed && *closed == 1);
    assert(!unfinished->publish(3).value);
    assert(broker.available() == 1);
    unfinished.reset();
    assert(broker.available() == 2);
    assert(!broker.activate(worker));
    assert(!broker.activate(restarted));
    assert(broker.activate(Identity{2,3}));
    auto newRequest = broker.begin(Identity{2,3});
    assert(newRequest);
    std::memcpy(newRequest->data(), "new", 3);
    const auto newHandle = newRequest->publish(3);
    assert(newHandle.value && newHandle.value != moved->value);
    assert(!broker.borrow(worker, newHandle));
    assert(!broker.borrow(restarted, newHandle));
    bool saw = false;
    assert(broker.with_borrow(Identity{2,3}, newHandle,
        [&](std::string_view view) {
            assert(view == "new");
            saw = true;
        }));
    assert(saw);
    assert(broker.close(Identity{2,3}, newHandle));
    // Callback unwinding must not leak a read loan or strand slot capacity.
    auto unwinding = broker.begin(supervisor);
    assert(unwinding);
    std::memcpy(unwinding->data(), "unwind", 6);
    const auto unwindHandle = unwinding->publish(6);
    bool unwound = false;
    try {
        broker.with_borrow(supervisor, unwindHandle, [&](std::string_view view) {
            assert(view == "unwind");
            throw 37;
        });
    } catch (int code) { unwound = code == 37; }
    assert(unwound);
    assert(broker.close(supervisor, unwindHandle));
    assert(broker.available() == 2);
    // Every short, authenticated request can reuse preallocated memory with
    // zero C++ new/new[] during the measured hot request processing cycle.
    auto before = news.load(std::memory_order_relaxed);
    for (int i = 0; i < 25000; ++i) {
        auto in = broker.begin(supervisor);
        assert(in);
        std::memcpy(in->data(), "ok", 2);
        const auto h = in->publish(2);
        bool entered = false;
        assert(broker.with_borrow(supervisor, h, [&](std::string_view view) {
            assert(view == "ok");
            entered = true;
        }));
        assert(entered);
        assert(broker.close(supervisor, h));
    }
    assert(news.load(std::memory_order_relaxed) == before);
    // Ensure no borrow can start after a concurrent retirement completes.
    const auto current = Identity{2,3};
    auto pending = broker.begin(current);
    assert(pending);
    std::memcpy(pending->data(), "racy", 4);
    const auto pendingHandle = pending->publish(4);
    auto view = broker.borrow(current, pendingHandle);
    assert(view);
    std::thread terminator([&] {
        auto count = broker.retire(current);
        assert(count && *count == 1);
    });
    terminator.join();
    assert(!broker.borrow(current, pendingHandle));
    assert(view->view() == "racy");
    view.reset();
    assert(broker.available() == 2);
    assert(broker.retire(supervisor).value() == 0);
    // Bounded incarnation history is NEVER evicted: fill the history for
    // distinct IDs and fail closed instead of resurrecting a forgotten ID.
    using SmallBroker = oreslang::runtime::ActorBoundRequestTable<1, 1, 8, 2>;
    SmallBroker small;
    assert(small.activate(SmallBroker::ActorIncarnation{41,1}));
    assert(small.retire(SmallBroker::ActorIncarnation{41,1}).value() == 0);
    assert(!small.activate(SmallBroker::ActorIncarnation{41,1})); // replay
    assert(small.activate(SmallBroker::ActorIncarnation{41,2})); // strictly newer
    assert(small.retire(SmallBroker::ActorIncarnation{41,2}).value() == 0);
    assert(small.activate(SmallBroker::ActorIncarnation{42,1}));
    assert(small.retire(SmallBroker::ActorIncarnation{42,1}).value() == 0);
    assert(!small.activate(SmallBroker::ActorIncarnation{43,1})); // history budget consumed
    assert(!small.activate(SmallBroker::ActorIncarnation{41,1}));
    assert(!small.activate(SmallBroker::ActorIncarnation{42,1}));
    assert(small.activate(SmallBroker::ActorIncarnation{41,3})); // existing ID still has authority
    assert(small.retire(SmallBroker::ActorIncarnation{41,3}).value() == 0);
    // Retire and borrow must linearize without recycling a buffer that a
    // concurrent reader already leased. Repeating fresh incarnations probes
    // races at several generations without expanding the authority budget.
    using RaceBroker = oreslang::runtime::ActorBoundRequestTable<1, 1, 32, 1>;
    RaceBroker raced;
    for (std::uint64_t generation = 1; generation <= 256; ++generation) {
        RaceBroker::ActorIncarnation actor{77, generation};
        assert(raced.activate(actor));
        auto incoming = raced.begin(actor);
        assert(incoming);
        std::memcpy(incoming->data(), "stable", 6);
        auto live = incoming->publish(6);
        assert(live.value);
        std::atomic<bool> beginRace{false}, wasRetired{false};
        std::thread reader([&] {
            while (!beginRace.load(std::memory_order_acquire))
                std::this_thread::yield();
            while (!wasRetired.load(std::memory_order_acquire)) {
                auto maybe = raced.borrow(actor, live);
                if (maybe) {
                    assert(maybe->view() == "stable");
                    std::this_thread::yield();
                    assert(maybe->view() == "stable");
                }
            }
        });
        std::thread revoker([&] {
            beginRace.store(true, std::memory_order_release);
            auto count = raced.retire(actor);
            assert(count && *count == 1);
            wasRetired.store(true, std::memory_order_release);
        });
        revoker.join();
        reader.join();
        assert(!raced.borrow(actor, live));
        assert(raced.available() == 1);
    }
    return 0;
}
