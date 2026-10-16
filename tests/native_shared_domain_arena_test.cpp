#include "oreslang/runtime/SharedDomainArena.hpp"
#include <atomic>
#include <array>
#include <cstdint>
#include <future>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <iostream>
using oreslang::runtime::SharedDomainArena;
void require(bool cond) { if (!cond) throw std::runtime_error("shared domain test failed"); }
template<typename F> void reject(F&& f) {
    bool denied=false;
    try { f(); } catch (const std::exception&) { denied=true; }
    require(denied);
}
int main() {
    static_assert(!std::is_move_constructible_v<SharedDomainArena::Lease>);
    SharedDomainArena a;
    auto writerGrant=a.grant(101, true, true);
    auto readerGrant=a.grant(102, true, false);
    auto other=SharedDomainArena{};
    auto foreignGrant=other.grant(104,true,false);
    reject([&] { (void)a.read(104, foreignGrant); });
    reject([&] { (void)a.write(102, writerGrant); }); // wrong actor, valid grant
    reject([&] { (void)a.read(0, writerGrant); }); // supervisor sentinel cannot bypass actor lease
    SharedDomainArena::Handle h{};
    {
        auto writer=a.write(101, writerGrant);
        h=writer.allocate(sizeof(std::uint64_t));
        std::uint64_t x=0x1234567890abcdefull;
        writer.write(h, &x,sizeof(x));
        reject([&] { writer.read(h, &x,sizeof(x)-1); });
    }
    std::atomic<std::uint64_t> observed{0};
    std::thread otherCarrier([&] {
        auto r=a.read(102, readerGrant);
        std::uint64_t x=0;
        r.read(h,&x,sizeof(x));
        observed.store(x);
        reject([&] { r.write(h,&x,sizeof(x)); });
    });
    otherCarrier.join();
    require(observed == 0x1234567890abcdefull);
    reject([&] { (void)a.write(102, readerGrant); });
    {
        auto outstanding = a.read(101, writerGrant);
        require(!a.try_revoke(readerGrant));  // must not block on a live reader
    }
    require(a.try_revoke(readerGrant));
    reject([&] { (void)a.read(102, readerGrant); });
    // Retirement is nonblocking and does not free underneath an active lease.
    {
        auto pending=a.read(101, writerGrant);
        require(!a.try_retire());
        std::uint64_t x=0;
        reject([&] { pending.read(h,&x,sizeof(x)); });
    }
    require(a.try_retire());
    require(a.try_retire());
    reject([&] { (void)a.read(101, writerGrant); });
    reject([&] { (void)a.grant(3,true,true); });
    require(other.try_retire());
    // Reader/writer contention and a 4,096-generation ABA regression. This
    // exercises real OS threads and checks every observed value is fully read.
    SharedDomainArena concurrent;
    auto wg = concurrent.grant(201, true, true);
    auto rg = concurrent.grant(202, true, false);
    SharedDomainArena::Handle ch{};
    {
        auto writer = concurrent.write(201, wg);
        ch = writer.allocate(sizeof(std::uint64_t));
        std::uint64_t value = 0;
        writer.write(ch, &value, sizeof(value));
        for (int i = 0; i < 4096; ++i) {
            auto transient = writer.allocate(4);
            const auto stale = transient;
            writer.drop(transient);
            reject([&] { std::uint32_t tmp; writer.read(stale, &tmp, sizeof(tmp)); });
            transient = writer.allocate(4);
            reject([&] { std::uint32_t tmp; writer.read(stale, &tmp, sizeof(tmp)); });
            writer.drop(transient);
        }
    }
    std::atomic<bool> start{false};
    std::atomic<bool> correct{true};
    std::thread writerThread([&] {
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        for (std::uint64_t i = 1; i <= 1500; ++i) {
            auto lease = concurrent.write(201, wg);
            lease.write(ch, &i, sizeof(i));
        }
    });
    std::array<std::thread,4> readers;
    for (auto& thread : readers) thread = std::thread([&] {
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        for (int i = 0; i < 1500; ++i) {
            auto lease = concurrent.read(202, rg);
            std::uint64_t value = 0;
            lease.read(ch, &value, sizeof(value));
            if (value > 1500) correct.store(false);
        }
    });
    start.store(true, std::memory_order_release);
    writerThread.join();
    for (auto& reader : readers) reader.join();
    require(correct.load());
    require(concurrent.try_revoke(rg));
    reject([&] { (void)concurrent.read(202, rg); });
    require(concurrent.try_retire());
        std::cout << "shared domain arena tests passed\n";
}
