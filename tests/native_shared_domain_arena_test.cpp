#include "oreslang/runtime/SharedDomainArena.hpp"
#include <atomic>
#include <array>
#include <algorithm>
#include <vector>
#include <cstdint>
#include <future>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <iostream>
#include <string_view>
using oreslang::runtime::SharedDomainArena;
using oreslang::runtime::detail::claim_nonrecycling_carrier;
using oreslang::runtime::detail::current_carrier_identity;
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

    // Carrier identity is a non-recycling process-lifetime token, not the
    // implementation's reusable std::thread::id. Sequential thread creation
    // intentionally maximizes the chance that native thread IDs are recycled.
    require(current_carrier_identity() == current_carrier_identity());
    constexpr std::size_t CarrierThreads = 512;
    std::array<std::uint64_t, CarrierThreads> carrierIds{};
    for (std::size_t i = 0; i < CarrierThreads; ++i) {
        std::thread carrier([&, i] {
            carrierIds[i] = current_carrier_identity();
        });
        carrier.join();
    }
    auto sortedCarrierIds = carrierIds;
    std::sort(sortedCarrierIds.begin(), sortedCarrierIds.end());
    require(sortedCarrierIds.front() != 0);
    require(std::adjacent_find(sortedCarrierIds.begin(), sortedCarrierIds.end()) ==
            sortedCarrierIds.end());

    constexpr auto maxCarrier = std::numeric_limits<std::uint64_t>::max();
    std::atomic<std::uint64_t> nearCarrierEnd{maxCarrier - 1};
    require(claim_nonrecycling_carrier(nearCarrierEnd) == maxCarrier - 1);
    require(nearCarrierEnd.load() == maxCarrier);
    reject([&] { (void)claim_nonrecycling_carrier(nearCarrierEnd); });
    require(nearCarrierEnd.load() == maxCarrier);
    // Domain IDs must remain unique across concurrent arena construction.
    // This exercises the real header allocator, not a synthetic copy.
    {
        constexpr std::size_t Threads = 8, PerThread = 256;
        std::array<std::array<std::uint64_t, PerThread>, Threads> issued{};
        std::array<std::thread, Threads> workers;
        std::atomic<bool> start{false};
        for (std::size_t t = 0; t < Threads; ++t) {
            workers[t] = std::thread([&, t] {
                while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
                for (std::size_t i = 0; i < PerThread; ++i) {
                    SharedDomainArena domain;
                    auto grant = domain.grant(1000 + t, 1, true, false);
                    issued[t][i] = grant.domain;
                    require(domain.try_retire());
                }
            });
        }
        start.store(true, std::memory_order_release);
        for (auto& worker : workers) worker.join();
        std::vector<std::uint64_t> values;
        values.reserve(Threads * PerThread);
        for (const auto& row : issued) for (auto identity : row) {
            require(identity != 0 && identity != std::numeric_limits<std::uint64_t>::max());
            values.push_back(identity);
        }
        std::sort(values.begin(), values.end());
        require(std::adjacent_find(values.begin(), values.end()) == values.end());
    }
    SharedDomainArena a;
    auto writerGrant=a.grant(101, 1, true, true);
    auto readerGrant=a.grant(102, 1, true, false);
    auto other=SharedDomainArena{};
    auto foreignGrant=other.grant(104, 1, true,false);
    reject([&] { (void)a.read(104, 1, foreignGrant); });
    reject([&] { (void)a.write(102, 1, writerGrant); }); // wrong actor, valid grant
    reject([&] { (void)a.read(101, 2, writerGrant); }); // same actor ID after restart is NOT authority
    reject([&] { (void)a.write(101, 2, writerGrant); }); // incarnation fences writers too
    reject([&] { (void)a.grant(101, 0, true, true); }); // no ambiguous incarnation
    reject([&] { (void)a.read(0, 1, writerGrant); }); // supervisor sentinel cannot bypass actor lease
    SharedDomainArena::Handle h{};
    {
        auto writer=a.write(101, 1, writerGrant);
        h=writer.allocate(sizeof(std::uint64_t));
        std::uint64_t x=0x1234567890abcdefull;
        writer.write(h, &x,sizeof(x));
        // A trusted lease object may be referenced from another OS thread,
        // but the lock-holding carrier is the only one authorized to use it.
        std::atomic<bool> foreignReadDenied{false}, foreignWriteDenied{false};
        std::atomic<bool> foreignAllocateDenied{false}, foreignDropDenied{false};
        std::thread foreignCarrier([&] {
            std::uint64_t probe = 0;
            try { writer.read(h, &probe, sizeof(probe)); }
            catch (const std::logic_error&) { foreignReadDenied.store(true); }
            try { writer.write(h, &probe, sizeof(probe)); }
            catch (const std::logic_error&) { foreignWriteDenied.store(true); }
            try { (void)writer.allocate(sizeof(probe)); }
            catch (const std::logic_error&) { foreignAllocateDenied.store(true); }
            try { writer.drop(h); }
            catch (const std::logic_error&) { foreignDropDenied.store(true); }
        });
        foreignCarrier.join();
        require(foreignReadDenied.load() && foreignWriteDenied.load() &&
                foreignAllocateDenied.load() && foreignDropDenied.load());
        std::uint64_t afterForeignCarrier = 0;
        writer.read(h, &afterForeignCarrier, sizeof(afterForeignCarrier));
        require(afterForeignCarrier == x);
        reject([&] { writer.read(h, &x,sizeof(x)-1); });
        // Drop must physically scrub sensitive bytes before the slot can be reused,
        // not just revoke its generation. A new handle must see only zeros.
        auto secret = writer.allocate(sizeof(std::uint64_t));
        std::uint64_t sensitive = 0xfedcba9876543210ull;
        writer.write(secret, &sensitive, sizeof(sensitive));
        writer.drop(secret);
        reject([&] { writer.read(secret, &sensitive, sizeof(sensitive)); });
        auto recycled = writer.allocate(sizeof(std::uint64_t));
        std::uint64_t cleared = ~std::uint64_t{0};
        writer.read(recycled, &cleared, sizeof(cleared));
        require(cleared == 0);
        writer.drop(recycled);
    }
    std::atomic<std::uint64_t> observed{0};
    std::thread otherCarrier([&] {
        auto r=a.read(102, 1, readerGrant);
        std::uint64_t x=0;
        r.read(h,&x,sizeof(x));
        observed.store(x);
        reject([&] { r.write(h,&x,sizeof(x)); });
    });
    otherCarrier.join();
    require(observed == 0x1234567890abcdefull);
    reject([&] { (void)a.write(102, 1, readerGrant); });
    {
        std::promise<void> leaseReady, leaseRelease;
        auto ready = leaseReady.get_future();
        auto release = leaseRelease.get_future();
        std::thread reader([&] {
            auto outstanding = a.read(101, 1, writerGrant);
            leaseReady.set_value();
            release.wait();
        });
        ready.wait();
        require(!a.try_revoke(readerGrant));  // CONTROL must not block on a reader
        // CONTROL membership admission also must not park behind a lease.
        bool grantBusy = false;
        try { (void)a.grant(105, 1, true, false); }
        catch (const std::logic_error& e) {
            grantBusy = std::string_view(e.what()) == "shared region busy";
        }
        require(grantBusy);
        leaseRelease.set_value();
        reader.join();
    }
    require(a.try_revoke(readerGrant));
    auto newReader = a.grant(105, 1, true, false);
    require(a.try_revoke(newReader));
    reject([&] { (void)a.read(102, 1, readerGrant); });
    // Retirement is nonblocking and does not free underneath an active lease.
    {
        std::promise<void> leaseReady, leaseRelease;
        auto ready = leaseReady.get_future();
        auto release = leaseRelease.get_future();
        std::atomic<bool> revokedReadDenied{false};
        std::thread reader([&] {
            auto pending = a.read(101, 1, writerGrant);
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
    reject([&] { (void)a.read(101, 1, writerGrant); });
    reject([&] { (void)a.grant(3, 1, true,true); });
    require(other.try_retire());
    // Reader/writer contention and a 4,096-generation ABA regression. This
    // exercises real OS threads and checks every observed value is fully read.
    SharedDomainArena concurrent;
    auto wg = concurrent.grant(201, 1, true, true);
    auto rg = concurrent.grant(202, 1, true, false);
    SharedDomainArena::Handle ch{};
    {
        auto writer = concurrent.write(201, 1, wg);
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
            auto lease = concurrent.read(202, 1, rg);
            held.set_value();
            releaseFuture.wait();
        });
        heldFuture.wait();
        reject([&] { (void)concurrent.write(201, 1, wg); });
        release.set_value();
        holding.join();
    }
    std::atomic<bool> start{false};
    std::atomic<bool> correct{true};
    std::thread writerThread([&] {
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        for (std::uint64_t i = 1; i <= 1500; ++i) {
            eventually([&] {
                auto lease = concurrent.write(201, 1, wg);
                lease.write(ch, &i, sizeof(i));
            });
        }
    });
    std::array<std::thread,4> readers;
    for (auto& thread : readers) thread = std::thread([&] {
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        for (int i = 0; i < 1500; ++i) {
            eventually([&] {
                auto lease = concurrent.read(202, 1, rg);
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
    reject([&] { (void)concurrent.read(202, 1, rg); });
    require(concurrent.try_retire());
        std::cout << "shared domain arena tests passed\n";
}
