#include "oreslang/runtime/SharedDomainArena.hpp"
#include <atomic>
#include <array>
#include <cstdint>
#include <future>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <iostream>
#include <string_view>
using oreslang::runtime::SharedDomainArena;
void require(bool cond) { if (!cond) throw std::runtime_error("shared domain test failed"); }
// Only busy admission is retryable; ownership, revocation and invalid handles
// must fail permanently rather than being swallowed by the contention loop.
template<typename F> void eventually(F&& f) {
    for (unsigned i = 0; i < 1000000; ++i) {
        try { f(); return; }
        catch (const std::logic_error& e) {
            if (std::string_view(e.what()) != "shared region busy") throw;
            std::this_thread::yield();
        }
    }
    throw std::runtime_error("shared region lease admission starved");
}
template<typename F> void reject(F&& f) {
    bool denied=false;
    try { f(); } catch (const std::exception&) { denied=true; }
    require(denied);
}
int main() {
    static_assert(!std::is_move_constructible_v<SharedDomainArena::Lease>);
    SharedDomainArena a;
    auto writerGrant=a.grant(101, 7, true, true);
    auto readerGrant=a.grant(102, 7, true, false);
    auto other=SharedDomainArena{};
    auto foreignGrant=other.grant(104, 7,true,false);
    reject([&] { (void)a.read(104, 7, foreignGrant); });
    reject([&] { (void)a.write(102, 7, writerGrant); }); // wrong actor, valid grant
    reject([&] { (void)a.read(0, 7, writerGrant); }); // supervisor sentinel cannot bypass actor lease
    // A replacement actor with the SAME numeric ID must not inherit grants
    // from an old incarnation, even before CONTROL revokes that membership.
    reject([&] { (void)a.read(101, 8, writerGrant); });
    reject([&] { (void)a.write(101, 8, writerGrant); });
    reject([&] { (void)a.grant(101, 0, true, true); });
    SharedDomainArena::Handle h{};
    {
        auto writer=a.write(101, 7, writerGrant);
        h=writer.allocate(sizeof(std::uint64_t));
        std::uint64_t x=0x1234567890abcdefull;
        writer.write(h, &x,sizeof(x));
        reject([&] { writer.read(h, &x,sizeof(x)-1); });
    }
    std::atomic<std::uint64_t> observed{0};
    std::thread otherCarrier([&] {
        auto r=a.read(102, 7, readerGrant);
        std::uint64_t x=0;
        r.read(h,&x,sizeof(x));
        observed.store(x);
        reject([&] { r.write(h,&x,sizeof(x)); });
    });
    otherCarrier.join();
    require(observed == 0x1234567890abcdefull);
    reject([&] { (void)a.write(102, 7, readerGrant); });
    {
        std::promise<void> leaseReady, leaseRelease;
        auto ready = leaseReady.get_future();
        auto release = leaseRelease.get_future();
        std::thread reader([&] {
            auto outstanding = a.read(101, 7, writerGrant);
            leaseReady.set_value();
            release.wait();
        });
        ready.wait();
        require(!a.try_revoke(readerGrant));  // CONTROL must not block on a reader
        // CONTROL membership admission also must not park behind a lease.
        bool grantBusy = false;
        try { (void)a.grant(105, 7, true, false); }
        catch (const std::logic_error& e) {
            grantBusy = std::string_view(e.what()) == "shared region busy";
        }
        require(grantBusy);
        leaseRelease.set_value();
        reader.join();
    }
    require(a.try_revoke(readerGrant));
    auto newReader = a.grant(105, 7, true, false);
    require(a.try_revoke(newReader));
    reject([&] { (void)a.read(102, 7, readerGrant); });
    // Retirement is nonblocking and does not free underneath an active lease.
    {
        std::promise<void> leaseReady, leaseRelease;
        auto ready = leaseReady.get_future();
        auto release = leaseRelease.get_future();
        std::atomic<bool> revokedReadDenied{false};
        std::thread reader([&] {
            auto pending = a.read(101, 7, writerGrant);
            leaseReady.set_value();
            release.wait();
            std::uint64_t x = 0;
            try { pending.read(h, &x, sizeof(x)); }
            catch (const std::logic_error&) { revokedReadDenied.store(true); }
        });
        ready.wait();
        require(!a.try_retire());
        leaseRelease.set_value();
        reader.join();
        require(revokedReadDenied.load());
    }
    require(a.try_retire());
    require(a.try_retire());
    reject([&] { (void)a.read(101, 7, writerGrant); });
    reject([&] { (void)a.grant(3, 7,true,true); });
    require(other.try_retire());
    // Explicit actor-ID reuse is not member authority reuse. Revoke/reissue
    // must preserve the incarnation fence in BOTH directions.
    SharedDomainArena recycledActor;
    auto previousGrant = recycledActor.grant(501, 17, true, true);
    reject([&] { (void)recycledActor.read(501, 18, previousGrant); });
    reject([&] { (void)recycledActor.write(501, 18, previousGrant); });
    {
        auto previousWriter = recycledActor.write(501, 17, previousGrant);
        auto oldHandle = previousWriter.allocate(sizeof(std::uint64_t));
        std::uint64_t oldValue = 42;
        previousWriter.write(oldHandle, &oldValue, sizeof(oldValue));
    }
    require(recycledActor.try_revoke(previousGrant));
    auto currentGrant = recycledActor.grant(501, 18, true, true);
    reject([&] { (void)recycledActor.read(501, 17, currentGrant); });
    reject([&] { (void)recycledActor.write(501, 17, currentGrant); });
    {
        auto currentWriter = recycledActor.write(501, 18, currentGrant);
        auto fresh = currentWriter.allocate(sizeof(std::uint64_t));
        std::uint64_t value = 73;
        currentWriter.write(fresh, &value, sizeof(value));
        currentWriter.read(fresh, &value, sizeof(value));
        require(value == 73);
    }
    require(recycledActor.try_revoke(currentGrant));
    require(recycledActor.try_retire());
    // Reader/writer contention and a 4,096-generation ABA regression. This
    // exercises real OS threads and checks every observed value is fully read.
    SharedDomainArena concurrent;
    auto wg = concurrent.grant(201, 7, true, true);
    auto rg = concurrent.grant(202, 7, true, false);
    SharedDomainArena::Handle ch{};
    {
        auto writer = concurrent.write(201, 7, wg);
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
    // A second carrier must return an explicit busy result instead of
    // parking its OS thread behind a live actor read lease.
    {
        std::promise<void> held, release;
        auto heldFuture = held.get_future();
        auto releaseFuture = release.get_future();
        std::thread holding([&] {
            auto lease = concurrent.read(202, 7, rg);
            held.set_value();
            releaseFuture.wait();
        });
        heldFuture.wait();
        reject([&] { (void)concurrent.write(201, 7, wg); });
        release.set_value();
        holding.join();
    }
    std::atomic<bool> start{false};
    std::atomic<bool> correct{true};
    std::thread writerThread([&] {
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        for (std::uint64_t i = 1; i <= 1500; ++i) {
            eventually([&] {
                auto lease = concurrent.write(201, 7, wg);
                lease.write(ch, &i, sizeof(i));
            });
        }
    });
    std::array<std::thread,4> readers;
    for (auto& thread : readers) thread = std::thread([&] {
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        for (int i = 0; i < 1500; ++i) {
            eventually([&] {
                auto lease = concurrent.read(202, 7, rg);
                std::uint64_t value = 0;
                lease.read(ch, &value, sizeof(value));
                if (value > 1500) correct.store(false);
            });
        }
    });
    start.store(true, std::memory_order_release);
    writerThread.join();
    for (auto& reader : readers) reader.join();
    require(correct.load());
    require(concurrent.try_revoke(rg));
    reject([&] { (void)concurrent.read(202, 7, rg); });
    require(concurrent.try_retire());
        std::cout << "shared domain arena tests passed\n";
}
