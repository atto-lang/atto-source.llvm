#include "oreslang/runtime/ActorResourceRegistry.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
// Release CTest builds set NDEBUG; contract assertions must still execute.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

using oreslang::runtime::ActorResourceRegistry;
using Kind = ActorResourceRegistry::Kind;
using Reclaim = ActorResourceRegistry::Reclaim;
using Phase = ActorResourceRegistry::Phase;

static_assert(!std::is_default_constructible_v<ActorResourceRegistry::Ticket>);
static_assert(!std::is_aggregate_v<ActorResourceRegistry::Ticket>);
static_assert(!std::is_constructible_v<ActorResourceRegistry::Ticket,
        std::uint64_t, std::uint64_t, std::uint64_t,
        ActorResourceRegistry::Activity>);

int main() {
    ActorResourceRegistry registry(15, 7, 4);
    std::vector<int> closed;
    registry.register_resource(Kind::socket, [&] {
        assert(registry.phase() == Phase::closing); // verifies no registry lock held
        assert(registry.try_reclaim().status == Reclaim::closing);
        closed.push_back(1);
    });
    registry.register_resource(Kind::file, [&] {
        closed.push_back(2);
        throw std::runtime_error("close failure");
    });
    registry.register_resource(Kind::ffi_callback, [&] { closed.push_back(3); });

    assert(registry.try_reclaim().status == Reclaim::not_retired);
    auto turn = registry.acquire_turn();
    auto loan = registry.acquire_loan();
    auto callback = registry.acquire_callback();
    registry.retire();
    registry.retire();
    assert(registry.try_reclaim().status == Reclaim::busy);
    registry.release_turn(turn);
    registry.release_callback(callback);
    assert(registry.try_reclaim().status == Reclaim::busy);
    registry.release_loan(loan);
    auto result = registry.try_reclaim();
    assert(result.status == Reclaim::failed);
    assert(registry.phase() == Phase::failed);
    assert(result.closed == 2);
    assert(result.attempted == 3);
    assert(registry.quarantined_resources() == 3);
    assert(result.errors.size() == 1);
    assert(result.failures == 1);
    assert(result.errors[0] == "close failure");
    assert((closed == std::vector<int>{3, 2, 1}));
    assert(registry.try_reclaim().status == Reclaim::failed);
    assert(registry.try_reclaim().closed == 2);
    assert(registry.try_reclaim().attempted == 3);
    assert(closed.size() == 3);

    bool rejected = false;
    try { registry.register_resource(Kind::timer, [] {}); }
    catch (const std::logic_error&) { rejected = true; }
    assert(rejected);
    rejected = false;
    try { registry.release_loan(loan); }
    catch (const std::logic_error&) { rejected = true; }
    assert(rejected);
    rejected = false;
    try { registry.acquire_callback(); }
    catch (const std::logic_error&) { rejected = true; }
    assert(rejected);

    // Cross-incarnation and cross-role tickets must never release another
    // actor's active memory loans, even when nonce values happen to match.
    ActorResourceRegistry first(21, 1);
    ActorResourceRegistry second(22, 1);
    auto owned = first.acquire_loan();
    auto foreign = second.acquire_loan();
    rejected = false;
    try { first.release_loan(foreign); }
    catch (const std::logic_error&) { rejected = true; }
    assert(rejected);
    rejected = false;
    try { first.release_callback(owned); }
    catch (const std::logic_error&) { rejected = true; }
    assert(rejected);
    first.retire();
    assert(first.try_reclaim().status == Reclaim::busy);
    first.release_loan(owned);
    assert(first.try_reclaim().status == Reclaim::complete);
    second.release_loan(foreign);
    second.retire();
    assert(second.try_reclaim().status == Reclaim::complete);

    // A malicious or buggy native closer can return huge error text; keep
    // diagnostics bounded and still attempt every remaining registered close.
    ActorResourceRegistry boundedErrors(25, 1, 3);
    std::string hugeError(8192, 'x');
    int successfulClosers = 0;
    boundedErrors.register_resource(Kind::file, [&] { ++successfulClosers; });
    boundedErrors.register_resource(Kind::socket, [&] { throw std::runtime_error(hugeError); });
    boundedErrors.register_resource(Kind::timer, [&] { ++successfulClosers; });
    boundedErrors.retire();
    auto boundedResult = boundedErrors.try_reclaim();
    assert(boundedResult.status == Reclaim::failed);
    assert(boundedErrors.phase() == Phase::failed);
    assert(boundedResult.closed == 2);
    assert(boundedResult.attempted == 3);
    assert(boundedResult.failures == 1);
    assert(boundedResult.errors.size() == 1);
    assert(boundedResult.errors[0].size() == 256);
    assert(successfulClosers == 2);
    assert(boundedErrors.try_reclaim().status == Reclaim::failed);
    assert(boundedErrors.try_reclaim().failures == 1);
    assert(successfulClosers == 2);  // terminal failure must never re-close

    // Arena protection takes priority over naive reverse registration:
    // even an allocation registered after an FFI revoker is freed last.
    ActorResourceRegistry ordered(29, 1);
    std::vector<int> order;
    ordered.register_resource(Kind::ffi_callback, [&] { order.push_back(1); });
    ordered.register_resource(Kind::native_allocation, [&] { order.push_back(2); });
    ordered.retire();
    auto orderedResult = ordered.try_reclaim();
    assert(orderedResult.status == Reclaim::complete);
    assert(ordered.quarantined_resources() == 0);
    assert((order == std::vector<int>{1, 2}));

    ActorResourceRegistry quarantine(30, 1);
    int allocationDrops = 0;
    auto nativeOwnership = std::make_shared<int>(42);
    std::weak_ptr<int> ownerProbe = nativeOwnership;
    quarantine.register_resource(Kind::ffi_callback, [] {
        throw std::runtime_error("callback revocation failed");
    });
    quarantine.register_resource(Kind::native_allocation,
                                 [&, owner = nativeOwnership] { ++allocationDrops; });
    nativeOwnership.reset();
    quarantine.retire();
    auto quarantineResult = quarantine.try_reclaim();
    assert(quarantineResult.status == Reclaim::failed);
    assert(quarantineResult.failures == 1);
    assert(quarantineResult.closed == 0);
    assert(quarantineResult.attempted == 1);
    assert(quarantineResult.skipped == 1);
    assert(quarantine.quarantined_resources() == 2);
    assert(!ownerProbe.expired()
           && "quarantine must retain captured arena ownership until supervisor release");
    assert(allocationDrops == 0);
    assert(quarantine.try_reclaim().status == Reclaim::failed);
    assert(allocationDrops == 0);

    // An arena destructor that itself fails must fence all remaining arenas.
    ActorResourceRegistry failedDrop(32, 1);
    int dependentDrops = 0;
    failedDrop.register_resource(Kind::native_allocation, [&] { ++dependentDrops; });
    failedDrop.register_resource(Kind::native_allocation, [] {
        throw std::runtime_error("native drop failed");
    });
    failedDrop.retire();
    auto failedDropResult = failedDrop.try_reclaim();
    assert(failedDropResult.status == Reclaim::failed);
    assert(failedDropResult.attempted == 1);
    assert(failedDropResult.skipped == 1);
    assert(failedDrop.quarantined_resources() == 2);
    assert(dependentDrops == 0);

    ActorResourceRegistry concurrent(16, 1, 2);
    std::atomic<int> finishes{0};
    concurrent.register_resource(Kind::native_allocation, [&] { ++finishes; });
    auto held = concurrent.acquire_loan();
    concurrent.retire();
    std::thread borrower([&] { concurrent.release_loan(held); });
    borrower.join();
    assert(concurrent.try_reclaim().status == Reclaim::complete);
    assert(concurrent.phase() == Phase::reclaimed);
    assert(finishes == 1);

    // In-flight trusted close must be observed as CLOSING by concurrent
    // reclaimers. Only one thread may invoke the registered closer.
    for (std::uint64_t incarnation = 1; incarnation <= 32; ++incarnation) {
        ActorResourceRegistry racing(31, incarnation);
        std::atomic<bool> entered{false};
        std::atomic<bool> released{false};
        std::atomic<int> called{0};
        racing.register_resource(Kind::file, [&] {
            entered.store(true, std::memory_order_release);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (!released.load(std::memory_order_acquire)
                    && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::yield();
            }
            ++called;
        });
        racing.retire();
        std::thread winner([&] { assert(racing.try_reclaim().status == Reclaim::complete); });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!entered.load(std::memory_order_acquire)
                && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        assert(entered.load(std::memory_order_acquire));
        assert(racing.try_reclaim().status == Reclaim::closing);
        released.store(true, std::memory_order_release);
        winner.join();
        assert(racing.try_reclaim().status == Reclaim::complete);
        assert(called == 1);
    }

    ActorResourceRegistry bounded(17, 1, 1);
    bounded.register_resource(Kind::socket, [] {});
    rejected = false;
    try { bounded.register_resource(Kind::file, [] {}); }
    catch (const std::length_error&) { rejected = true; }
    assert(rejected);
    bounded.retire();
    assert(bounded.try_reclaim().status == Reclaim::complete);

    ActorResourceRegistry tokenBound(18, 1, 1);
    std::vector<ActorResourceRegistry::Ticket> outstanding;
    outstanding.reserve(4096);
    for (int i = 0; i < 4096; ++i) outstanding.push_back(tokenBound.acquire_loan());
    rejected = false;
    try { tokenBound.acquire_loan(); }
    catch (const std::length_error&) { rejected = true; }
    assert(rejected);
    tokenBound.retire();
    assert(tokenBound.try_reclaim().status == Reclaim::busy);
    for (auto token : outstanding) tokenBound.release_loan(token);
    assert(tokenBound.try_reclaim().status == Reclaim::complete);
}
