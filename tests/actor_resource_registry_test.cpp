#include "oreslang/runtime/ActorResourceRegistry.hpp"
#include <algorithm>
#include <atomic>
// Release CTest builds set NDEBUG; contract assertions must still execute.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <stdexcept>
#include <thread>
#include <vector>

using oreslang::runtime::ActorResourceRegistry;
using Kind = ActorResourceRegistry::Kind;
using Reclaim = ActorResourceRegistry::Reclaim;
using Phase = ActorResourceRegistry::Phase;

int main() {
    ActorResourceRegistry registry(15, 7, 4);
    std::vector<int> closed;
    registry.register_resource(Kind::socket, [&] {
        assert(registry.phase() == Phase::closing); // verifies no registry lock held
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
    assert(result.status == Reclaim::complete);
    assert(result.closed == 3);
    assert(result.errors.size() == 1);
    assert(result.errors[0] == "close failure");
    assert((closed == std::vector<int>{3, 2, 1}));
    assert(registry.try_reclaim().closed == 3);
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

    ActorResourceRegistry concurrent(16, 1, 2);
    std::atomic<int> finishes{0};
    concurrent.register_resource(Kind::native_allocation, [&] { ++finishes; });
    auto held = concurrent.acquire_loan();
    concurrent.retire();
    std::thread borrower([&] { concurrent.release_loan(held); });
    borrower.join();
    assert(concurrent.try_reclaim().status == Reclaim::complete);
    assert(finishes == 1);

    ActorResourceRegistry bounded(17, 1, 1);
    bounded.register_resource(Kind::socket, [] {});
    rejected = false;
    try { bounded.register_resource(Kind::file, [] {}); }
    catch (const std::length_error&) { rejected = true; }
    assert(rejected);
    bounded.retire();
    assert(bounded.try_reclaim().status == Reclaim::complete);
}
