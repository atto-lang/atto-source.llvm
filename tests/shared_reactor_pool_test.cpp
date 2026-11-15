#include "SharedReactorPool.hpp"
#include <sys/eventfd.h>
#include <poll.h>
#include <unistd.h>
#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <set>
#include <vector>

using Pool = oreslang::runtime::SharedReactorPool;
static void check(bool value) { if (!value) std::abort(); }
template<class Predicate> static void until(Predicate predicate) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!predicate()) {
        check(std::chrono::steady_clock::now() < deadline); std::this_thread::yield();
    }
}
static thread_local bool in_poll = false;
struct Reactor final : Pool::Reactor {
    int signal = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    int input = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    std::atomic<Pool*> pool{nullptr};
    std::atomic<int> active{0}, calls{0}, deliveries{0}, wakes{0};
    std::mutex ids_gate;
    std::set<std::thread::id> poll_ids, work_ids;
    std::uint64_t pending = 0; // only polling-lease owner accesses this
    Reactor() { check(signal >= 0 && input >= 0); }
    ~Reactor() { ::close(signal); ::close(input); }
    static void notify(int fd) noexcept {
        std::uint64_t one = 1; ssize_t n;
        do { n = write(fd, &one, sizeof(one)); } while (n < 0 && errno == EINTR);
        check(n == sizeof(one) || (n < 0 && errno == EAGAIN));
    }
    void wake() noexcept override { ++wakes; notify(signal); }
    void emit() { notify(input); }
    static void delivery(void* argument) {
        auto& self = *static_cast<Reactor*>(argument);
        check(!in_poll);
        { std::lock_guard<std::mutex> lock(self.ids_gate); self.work_ids.insert(std::this_thread::get_id()); }
        ++self.deliveries;
    }
    void poll(bool wait) noexcept override {
        check(active.fetch_add(1) == 0); in_poll = true; ++calls;
        { std::lock_guard<std::mutex> lock(ids_gate); poll_ids.insert(std::this_thread::get_id()); }
        pollfd fds[2]{{signal, POLLIN, 0}, {input, POLLIN, 0}};
        int result;
        do { result = ::poll(fds, 2, wait && pending == 0 ? 10 : 0); }
        while (result < 0 && errno == EINTR);
        check(result >= 0);
        std::uint64_t n = 0;
        if (fds[0].revents & POLLIN) check(read(signal, &n, sizeof(n)) == sizeof(n));
        if (fds[1].revents & POLLIN) {
            check(read(input, &n, sizeof(n)) == sizeof(n)); pending += n;
        }
        auto* target = pool.load();
        for (unsigned quantum = 0; target && pending && quantum < 128; ++quantum) {
            if (!target->try_submit(delivery, this)) break;
            --pending;
        }
        in_poll = false; check(active.fetch_sub(1) == 1);
    }
};
static void increment(void* n) { check(!in_poll); ++*static_cast<std::atomic<int>*>(n); }
static void throws(void*) { throw std::runtime_error("expected"); }
struct Block { std::atomic<bool> entered{false}, release{false}; };
static void block(void* p) {
    auto& b = *static_cast<Block*>(p); b.entered = true;
    until([&] { return b.release.load(); });
}
struct SelfClose { Pool* pool; std::atomic<bool> denied{false}; };
static void self_close(void* p) {
    auto& x = *static_cast<SelfClose*>(p);
    try { x.pool->close(); } catch (const std::logic_error&) { x.denied = true; }
}
struct HotChain { Pool* pool; Reactor* reactor; std::atomic<int> slices{0}; };
static void hot_chain(void* p) {
    auto& chain = *static_cast<HotChain*>(p);
    if (++chain.slices == 1) chain.reactor->emit();
    if (chain.reactor->deliveries == 0) check(chain.pool->try_submit(hot_chain, p));
}
// Deliberately broken polling adapter must not escape across a worker thread.
struct FailingReactor final : Pool::Reactor {
    std::atomic<bool> fail{false};
    void poll(bool may_wait) override {
        if (fail.load(std::memory_order_acquire)) throw std::runtime_error("poll failure");
        if (may_wait) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    void wake() noexcept override {}
};
int main() {
    // Default is exactly two pool workers, not two plus a selector/helper.
    {
        Reactor reactor; Pool pool(reactor, 2, 4); reactor.pool = &pool;
        check(pool.started_workers() == 2);
        for (int i = 0; i < 100; ++i) reactor.emit();
        until([&] { return reactor.deliveries == 100; });
        check(pool.started_workers() == 2); // pending I/O never expands workers
        check(pool.try_submit(throws, nullptr));
        SelfClose self{&pool}; check(pool.try_submit(self_close, &self));
        until([&] { return self.denied.load() && pool.failures() == 1; });
        pool.ensure_workers(4); check(pool.started_workers() == 4);
        bool denied = false;
        try { pool.ensure_workers(5); } catch (const std::invalid_argument&) { denied = true; }
        check(denied);
        std::atomic<int> done{0};
        std::vector<std::thread> producers;
        for (int i = 0; i < 4; ++i) producers.emplace_back([&] {
            for (int j = 0; j < 2000; ++j)
                while (!pool.try_submit(increment, &done)) std::this_thread::yield();
        });
        for (auto& p : producers) p.join();
        until([&] { return done == 8000; });
        pool.close(); check(!pool.try_submit(increment, &done));
        check(pool.completed() == 8102);
        for (auto id : reactor.work_ids) reactor.poll_ids.insert(id);
        check(reactor.poll_ids.size() <= 4); check(reactor.active == 0);
    }
    // A permanently runnable queue cannot prevent the sole worker polling I/O.
    {
        Reactor reactor; Pool pool(reactor, 1, 1); reactor.pool = &pool;
        HotChain chain{&pool, &reactor}; check(pool.try_submit(hot_chain, &chain));
        until([&] { return reactor.deliveries == 1; });
        pool.close(); check(chain.slices >= 64);
        check(reactor.poll_ids.size() == 1 && reactor.work_ids.size() == 1);
        check(*reactor.poll_ids.begin() == *reactor.work_ids.begin());
    }
    // One-worker mode, bounded saturation and graceful accepted-work draining.
    {
        Reactor reactor; Pool pool(reactor, 1, 1); reactor.pool = &pool;
        Block b; check(pool.try_submit(block, &b)); until([&] { return b.entered.load(); });
        std::atomic<int> done{0};
        const auto wakes_before_hot_queue = reactor.wakes.load();
        for (std::size_t i = 0; i < Pool::queue_capacity; ++i) check(pool.try_submit(increment, &done));
        check(reactor.wakes == wakes_before_hot_queue); // executor busy, no polling lease
        check(!pool.try_submit(increment, &done));
        b.release = true; pool.close(); check(done == 1024); check(pool.completed() == 1025);
    }
    // After the first poll exception, seal admission and drain every previously
    // accepted task without polling the broken reactor again.
    {
        FailingReactor reactor; Pool pool(reactor, 1, 1);
        Block blocked;
        check(pool.try_submit(block, &blocked));
        until([&] { return blocked.entered.load(); });
        std::atomic<int> done{0};
        for (int i = 0; i < 128; ++i) check(pool.try_submit(increment, &done));
        reactor.fail.store(true, std::memory_order_release);
        blocked.release = true;
        until([&] { return pool.failures() == 1; });
        pool.close();
        check(pool.failures() == 1 && done == 128 && pool.completed() == 129);
        check(!pool.try_submit(increment, &done));
    }
    {
        Reactor reactor; bool denied = false;
        try { Pool invalid(reactor, 0, 2); } catch (const std::invalid_argument&) { denied = true; }
        check(denied);
    }
    std::cout << "shared-reactor host: default-two, single-poller, real-fd readiness, shared execution, "
                 "bounded queue, 8000 concurrent submissions, growth cap, self-join denial, drain PASS\n";
}
