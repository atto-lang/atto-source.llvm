#include "oreslang/runtime/RequestSlotTable.hpp"
#if defined(__linux__)
#define ORES_TEST_TCP 1
#include "oreslang/runtime/NonblockingTcpLinux.hpp"
#endif
#include <atomic>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <new>
#include <thread>
#include <utility>

static std::atomic<std::size_t> allocations{0};
__attribute__((noinline)) void* operator new(std::size_t n) {
    allocations.fetch_add(1, std::memory_order_relaxed);
    if (auto* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
__attribute__((noinline)) void* operator new[](std::size_t n) {
    allocations.fetch_add(1, std::memory_order_relaxed);
    if (auto* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
__attribute__((noinline)) void operator delete(void* p) noexcept { std::free(p); }
__attribute__((noinline)) void operator delete[](void* p) noexcept { std::free(p); }
__attribute__((noinline)) void operator delete(void* p, std::size_t) noexcept { std::free(p); }
__attribute__((noinline)) void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

using Table = oreslang::runtime::RequestSlotTable<2, 128>;
int main() {
    Table table;
    assert(!table.begin(0));
    auto writable = table.begin(10);
    assert(writable);
    std::memcpy(writable->data(), "GET /hello", 10);
    assert(!writable->publish(129).value);
    const auto parent = writable->publish(10);
    assert(parent.value);
    assert(!writable->data());
    assert(!table.borrow(parent, 11));
    auto first = table.borrow(parent, 10);
    assert(first && first->view() == "GET /hello");
    assert(!table.transfer(parent, 10, 11)); // outstanding borrower
    assert(!table.close(parent, 11));
    first.reset();
    const auto moved = table.transfer(parent, 10, 11);
    assert(moved);
    assert(!table.borrow(parent, 10));
    assert(!table.close(parent, 10));
    auto kept = table.borrow(*moved, 11);
    assert(kept && kept->view() == "GET /hello");
    assert(table.close(*moved, 11));
    assert(!table.borrow(*moved, 11));
    assert(!table.close(*moved, 11));
    assert(table.available() == 1); // held view delays slot reuse
    kept.reset();
    assert(table.available() == 2);
    // Abandoned ingress (including move-assign) reclaims its slot.
    auto abandoned = table.begin(10);
    assert(abandoned);
    auto other = table.begin(10);
    assert(other);
    assert(!table.begin(10)); // bounded exhaustion
    abandoned = std::move(*other); // old ingress revoked
    assert(table.available() == 1);
    abandoned.reset();
    assert(table.available() == 2);
    // Allocation counter covers the hot request/borrow/handoff/retire cycle,
    // not preallocation, kernel networking, or compiler guest allocations.
    const auto before = allocations.load(std::memory_order_relaxed);
    for (int i = 0; i < 25000; ++i) {
        auto in = table.begin(1);
        assert(in);
        std::memcpy(in->data(), "abc", 3);
        const auto h = in->publish(3);
        auto view = table.borrow(h, 1);
        assert(view && view->view() == "abc");
        view.reset();
        assert(table.close(h, 1));
        assert(!table.borrow(h, 1));
    }
    assert(allocations.load(std::memory_order_relaxed) == before);
    // Cancellation cannot recycle memory while a reader holds it.
    auto write = table.begin(7);
    assert(write);
    std::memcpy(write->data(), "lifetime", 8);
    const auto h = write->publish(8);
    auto hold = table.borrow(h, 7);
    std::atomic<bool> started{false}, finished{false};
    std::thread closing([&] {
        started.store(true, std::memory_order_release);
        assert(table.close(h, 7));
        finished.store(true, std::memory_order_release);
    });
    while (!finished.load(std::memory_order_acquire)) std::this_thread::yield();
    assert(started.load(std::memory_order_acquire));
    assert(hold && hold->view() == "lifetime");
    assert(table.available() == 1);
    hold.reset();
    closing.join();
    assert(table.available() == 2);
    // Scoped native callback must retire its loan even if user code throws.
    // Calling close inside a live view revokes authority, not view memory.
    auto scopedIngress = table.begin(7);
    assert(scopedIngress);
    std::memcpy(scopedIngress->data(), "scoped", 6);
    const auto scoped = scopedIngress->publish(6);
    const auto scopedBefore = allocations.load(std::memory_order_relaxed);
    bool called = false;
    assert(table.with_borrow(scoped, 7, [&](std::string_view bytes) {
        called = true;
        assert(bytes == "scoped");
        assert(!table.transfer(scoped, 7, 9));
    }));
    assert(called);
    assert(allocations.load(std::memory_order_relaxed) == scopedBefore);
    assert(!table.with_borrow(scoped, 9, [](std::string_view) {}));
    bool caught = false;
    try {
        table.with_borrow(scoped, 7, [&](std::string_view bytes) {
            assert(bytes == "scoped");
            assert(table.close(scoped, 7));
            assert(table.available() == 1);
            throw 17; // tests unwind without allocating exception storage
        });
    } catch (int code) {
        caught = code == 17;
    }
    assert(caught && table.available() == 2);
    assert(!table.with_borrow(scoped, 7, [](std::string_view) {}));
#if defined(ORES_TEST_TCP)
    // Live loopback TCP -> transport slot -> handle-only actor handoff.
    using namespace oreslang::runtime;
    auto listener = NonblockingTcpListener::loopback(0, 16);
    auto client = OwnedSocketFd::create_tcp();
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(listener.local_port());
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const int connected = ::connect(client.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    assert(connected == 0 || (connected == -1 && errno == EINPROGRESS));
    EpollReadiness ep;
    ep.add(listener.fd(), EPOLLIN, 1);
    std::array<EpollReadiness::Ready, EpollReadiness::max_batch> events{};
    assert(ep.wait(events, 1500) > 0);
    auto accepted = listener.try_accept();
    assert(accepted.client.valid());
    auto ingress = table.begin(10);
    assert(ingress);
    const auto sent = try_write(client.get(), "GET /x\r\n", 8);
    assert(sent.disposition == IoDisposition::transferred && sent.size == 8);
    ep.add(accepted.client.get(), EPOLLIN, 2);
    std::size_t total = 0;
    while (total < 8) {
        const auto read = try_read(accepted.client.get(), ingress->data() + total,
                                   ingress->capacity() - total);
        if (read.disposition == IoDisposition::transferred) {
            total += read.size;
        } else {
            assert(read.disposition == IoDisposition::would_block);
            assert(ep.wait(events, 1500) > 0);
            ep.rearm(accepted.client.get(), EPOLLIN, 2);
        }
    }
    const auto supervisor = ingress->publish(total);
    const auto actor = table.transfer(supervisor, 10, 20);
    assert(actor && !table.borrow(supervisor, 10));
    auto borrowed = table.borrow(*actor, 20);
    assert(borrowed && borrowed->view() == "GET /x\r\n");
    borrowed.reset();
    assert(table.close(*actor, 20));
    assert(table.available() == 2);
    ep.remove(accepted.client.get());
    ep.remove(listener.fd());
#endif
    return 0;
}
