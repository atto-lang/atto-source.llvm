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
#include <new>
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
    static_assert(!std::is_copy_constructible_v<SharedDomainArena::Control>);
    static_assert(!std::is_move_constructible_v<SharedDomainArena::Control>);
    static_assert(!std::is_copy_constructible_v<SharedDomainArena::LifetimePin>);
    static_assert(std::is_move_constructible_v<SharedDomainArena::LifetimePin>);
    static_assert(!std::is_aggregate_v<SharedDomainArena::Grant>);
    static_assert(!std::is_default_constructible_v<SharedDomainArena::Grant>);
    static_assert(!std::is_constructible_v<SharedDomainArena::Grant,
        std::uint64_t, std::uint64_t, std::size_t>);
    static_assert(!std::is_aggregate_v<SharedDomainArena::Handle>);
    static_assert(!std::is_constructible_v<SharedDomainArena::Handle,
        std::uint64_t, std::uint64_t, std::size_t>);

    // A stale CONTROL capability must not regain authority if allocator/stack
    // reuse places a later arena at the exact same address.
    {
        alignas(SharedDomainArena) std::byte storage[sizeof(SharedDomainArena)];
        auto* first = ::new (static_cast<void*>(storage)) SharedDomainArena();
        auto staleControl = first->take_control();
        require(first->try_retire(staleControl));
        first->~SharedDomainArena();

        auto* second = ::new (static_cast<void*>(storage)) SharedDomainArena();
        auto currentControl = second->take_control();
        reject([&] { (void)second->grant(staleControl, 999, 1, true, false); });
        auto currentGrant = second->grant(currentControl, 999, 1, true, false);
        require(second->try_revoke(currentControl, currentGrant));
        require(second->try_retire(currentControl));
        second->~SharedDomainArena();
    }
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
                    auto control = domain.take_control();
                    auto grant = domain.grant(control, 1000 + t, 1, true, false);
                    issued[t][i] = grant.domain_id();
                    require(domain.try_retire(control));
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
    auto control = a.take_control();
    reject([&] { (void)a.take_control(); });
    auto writerGrant=a.grant(control, 101, 1, true, true);
    // Old and restarted actor incarnations may not share live membership.
    reject([&] { (void)a.grant(control, 101, 1, true, false); });
    reject([&] { (void)a.grant(control, 101, 2, true, true); });
    auto readerGrant=a.grant(control, 102, 1, true, false);
    // A default invalid handle can exist for delayed assignment, but raw live
    // authority coordinates cannot be constructed outside the arena/lease.
    SharedDomainArena::Handle invalidHandle{};
    {
        auto writer = a.write(101, 1, writerGrant);
        std::uint64_t probe = 0;
        reject([&] { writer.read(invalidHandle, &probe, sizeof(probe)); });
    }
    auto other=SharedDomainArena{};
    auto otherControl=other.take_control();
    auto foreignGrant=other.grant(otherControl, 104, 1, true,false);
    reject([&] { (void)a.read(104, 1, foreignGrant); });
    reject([&] { (void)a.write(102, 1, writerGrant); }); // wrong actor, valid grant
    reject([&] { (void)a.read(101, 2, writerGrant); }); // same actor ID after restart is NOT authority
    reject([&] { (void)a.write(101, 2, writerGrant); }); // incarnation fences writers too
    reject([&] { (void)a.grant(control, 101, 0, true, true); }); // no ambiguous incarnation
    reject([&] { (void)a.grant(otherControl, 106, 1, true, false); }); // wrong domain CONTROL
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
        require(!a.try_revoke(control, readerGrant));  // CONTROL must not block on a reader
        // CONTROL membership admission also must not park behind a lease.
        bool grantBusy = false;
        try { (void)a.grant(control, 105, 1, true, false); }
        catch (const std::logic_error& e) {
            grantBusy = std::string_view(e.what()) == "shared region busy";
        }
        require(grantBusy);
        leaseRelease.set_value();
        reader.join();
    }
    require(a.try_revoke(control, readerGrant));
    auto newReader = a.grant(control, 105, 1, true, false);
    require(a.try_revoke(control, newReader));
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
        require(!a.try_retire(control));
        leaseRelease.set_value();
        reader.join();
        require(revokedReadDenied.load());
    }
    require(a.try_retire(control));
    require(a.try_retire(control));
    reject([&] { (void)a.read(101, 1, writerGrant); });
    reject([&] { (void)a.grant(control, 3, 1, true,true); });
    require(other.try_retire(otherControl));
    // Reader/writer contention and a 4,096-generation ABA regression. This
    // exercises real OS threads and checks every observed value is fully read.
    SharedDomainArena concurrent;
    auto concurrentControl = concurrent.take_control();
    auto wg = concurrent.grant(concurrentControl, 201, 1, true, true);
    auto rg = concurrent.grant(concurrentControl, 202, 1, true, false);
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
    require(concurrent.try_revoke(concurrentControl, rg));
    reject([&] { (void)concurrent.read(202, 1, rg); });
    require(concurrent.try_retire(concurrentControl));

    // Lifetime-only pins keep physical storage alive across continuation/FFI/I/O
    // completion without granting any read/write or slot authority.
    {
        SharedDomainArena pinned;
        auto pinnedControl = pinned.take_control();
        auto pinnedGrant = pinned.grant(pinnedControl, 301, 7, true, false);
        auto pin = pinned.pin(301, 7, pinnedGrant);
        require(pin.live());
        require(!pinned.try_revoke(pinnedControl, pinnedGrant));
        // A fresh incarnation must not replace a member while an old
        // continuation still pins that membership.
        reject([&] { (void)pinned.grant(pinnedControl, 301, 8, true, false); });
        require(!pinned.try_retire(pinnedControl));
        reject([&] { (void)pinned.read(301, 7, pinnedGrant); });
        std::thread completion([p = std::move(pin)]() mutable {
            require(p.live());
            p.release();
            require(!p.live());
            p.release();
        });
        completion.join();
        // STOP has sealed the domain; an old pin has drained so physical
        // retirement proceeds, but member grants cannot reopen afterwards.
        require(pinned.try_retire(pinnedControl));
        require(pinned.try_retire(pinnedControl));
        reject([&] { (void)pinned.pin(301, 7, pinnedGrant); });
    }

    // Duplicate completion callbacks may arrive on distinct carriers.
    // Exactly one thread consumes the same lifetime-only token; no release
    // may underflow the counter or permit early physical arena destruction.
    for (int round = 0; round < 64; ++round) {
        SharedDomainArena duplicateCallbacks;
        auto ownership = duplicateCallbacks.take_control();
        auto member = duplicateCallbacks.grant(ownership, 811, 1, true, false);
        auto token = duplicateCallbacks.pin(811, 1, member);
        require(!duplicateCallbacks.try_retire(ownership));
        std::array<std::thread, 8> callbacks;
        for (auto& callback : callbacks) callback = std::thread([&] {
            for (int duplicate = 0; duplicate < 8; ++duplicate) token.release();
        });
        for (auto& callback : callbacks) callback.join();
        require(!token.live());
        require(duplicateCallbacks.try_retire(ownership));
        require(duplicateCallbacks.try_retire(ownership));
    }

    // Per-member pin counts allow independent CONTROL revocation without
    // prematurely recycling a still-pinned actor's membership identity.
    {
        SharedDomainArena separateMembers;
        auto authority = separateMembers.take_control();
        auto old = separateMembers.grant(authority, 1201, 1, true, false);
        auto other = separateMembers.grant(authority, 1202, 1, true, false);
        auto outstanding = separateMembers.pin(1201, 1, old);
        require(!separateMembers.try_revoke(authority, old));
        require(separateMembers.try_revoke(authority, other));
        reject([&] { (void)separateMembers.grant(authority, 1201, 2, true, false); });
        outstanding.release();
        require(separateMembers.try_revoke(authority, old));
        auto restarted = separateMembers.grant(authority, 1201, 2, true, false);
        require(separateMembers.try_revoke(authority, restarted));
        require(separateMembers.try_retire(authority));
    }
    // Capacity, concurrent physical completion and duplicate callbacks.
    // The 65th live pin is rejected; its failure cannot charge another slot.
    for (int round = 0; round < 16; ++round) {
        SharedDomainArena bounded;
        auto control = bounded.take_control();
        auto member = bounded.grant(control, 8301, 1, true, false);
        std::vector<SharedDomainArena::LifetimePin> tokens;
        tokens.reserve(SharedDomainArena::MaxLifetimePins);
        for (std::size_t i = 0; i < SharedDomainArena::MaxLifetimePins; ++i)
            tokens.emplace_back(bounded.pin(8301, 1, member));
        reject([&] { (void)bounded.pin(8301, 1, member); });
        require(!bounded.try_revoke(control, member));
        std::atomic<bool> ready{false};
        std::array<std::thread, 8> finishers;
        for (std::size_t carrier = 0; carrier < finishers.size(); ++carrier) {
            finishers[carrier] = std::thread([&, carrier] {
                while (!ready.load(std::memory_order_acquire))
                    std::this_thread::yield();
                for (std::size_t i = carrier; i < tokens.size(); i += finishers.size()) {
                    tokens[i].release();
                    tokens[i].release();
                }
            });
        }
        ready.store(true, std::memory_order_release);
        for (auto& carrier : finishers) carrier.join();
        for (const auto& token : tokens) require(!token.live());
        require(bounded.try_revoke(control, member));
        require(bounded.try_retire(control));
        require(bounded.try_retire(control));
    }
    std::cout << "shared domain arena tests passed\n";
}
