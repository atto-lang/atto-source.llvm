#include "oreslang/runtime/ActorResourceRegistry.hpp"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <memory>
#include <new>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

using Registry = oreslang::runtime::ActorResourceRegistry;
using Activity = Registry::Activity;
using Ticket = Registry::Ticket;
using Reclaim = Registry::Reclaim;

// Construct in a separate translation unit as well: registry domains must
// share one linked counter, not restart at one in each including source file.
std::unique_ptr<Registry> make_peer_registry();

static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
static Ticket acquire(Registry& registry, Activity role) {
    if (role == Activity::turn) return registry.acquire_turn();
    if (role == Activity::loan) return registry.acquire_loan();
    if (role == Activity::callback) return registry.acquire_callback();
    return registry.acquire_handoff();
}
static void release(Registry& registry, Activity role, Ticket ticket) {
    if (role == Activity::turn) registry.release_turn(ticket);
    else if (role == Activity::loan) registry.release_loan(ticket);
    else if (role == Activity::callback) registry.release_callback(ticket);
    else registry.release_handoff(ticket);
}
static void rejects(Registry& registry, Activity role, Ticket ticket) {
    bool rejected = false;
    try { release(registry, role, ticket); }
    catch (const std::logic_error&) { rejected = true; }
    require(rejected, "foreign or replayed resource ticket released live activity");
}

static void same_identity(Activity role) {
    int closed = 0;
    Registry first(41, 7);
    auto second = make_peer_registry();
    first.register_resource(Registry::Kind::native_allocation, [&] { ++closed; });
    const auto owned = acquire(first, role);
    const auto copied = owned;
    const auto foreign = acquire(*second, role);
    first.retire();
    rejects(first, role, foreign);
    require(first.try_reclaim().status == Reclaim::busy && closed == 0,
            "foreign release permitted premature allocation reclamation");
    rejects(first, role == Activity::turn ? Activity::loan : Activity::turn, owned);
    require(first.try_reclaim().status == Reclaim::busy, "wrong-role release consumed a ticket");
    release(first, role, copied);
    rejects(first, role, owned);
    require(first.try_reclaim().status == Reclaim::complete && closed == 1,
            "own copied ticket failed to drain exactly once");
    require(first.try_reclaim().status == Reclaim::complete && closed == 1,
            "repeat reclamation reran allocation closer");
    release(*second, role, foreign);
    second->retire();
    require(second->try_reclaim().status == Reclaim::complete, "source registry did not drain");
}

static void same_address(Activity role) {
    alignas(Registry) std::byte storage[sizeof(Registry)];
    auto* old = new (storage) Registry(41, 7);
    const auto stale = acquire(*old, role);
    release(*old, role, stale);
    old->retire();
    require(old->try_reclaim().status == Reclaim::complete, "old registry did not drain");
    old->~Registry();
    auto* replacement = new (storage) Registry(41, 7);
    int closed = 0;
    replacement->register_resource(Registry::Kind::native_allocation, [&] { ++closed; });
    const auto current = acquire(*replacement, role);
    replacement->retire();
    rejects(*replacement, role, stale);
    require(replacement->try_reclaim().status == Reclaim::busy && closed == 0,
            "same-address stale ticket bypassed replacement pin");
    release(*replacement, role, current);
    require(replacement->try_reclaim().status == Reclaim::complete && closed == 1,
            "replacement registry did not reclaim exactly once");
    replacement->~Registry();
}

static void concurrent_instances(Activity role) {
    constexpr unsigned workers = 12, per_worker = 16;
    using Held = std::pair<std::unique_ptr<Registry>, Ticket>;
    std::array<std::vector<Held>, workers> batches;
    std::array<std::thread, workers> threads;
    std::array<std::exception_ptr, workers> failures{};
    std::atomic<unsigned> ready{0};
    std::atomic<bool> start{false};
    for (unsigned worker = 0; worker < workers; ++worker) {
        threads[worker] = std::thread([&, worker] {
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            try {
                auto& batch = batches[worker];
                batch.reserve(per_worker);
                for (unsigned i = 0; i < per_worker; ++i) {
                    auto registry = i % 2 ? make_peer_registry() : std::make_unique<Registry>(41, 7);
                    const auto ticket = acquire(*registry, role);
                    batch.emplace_back(std::move(registry), ticket);
                }
            } catch (...) { failures[worker] = std::current_exception(); }
        });
    }
    while (ready.load(std::memory_order_acquire) != workers) std::this_thread::yield();
    start.store(true, std::memory_order_release);
    for (auto& thread : threads) thread.join();
    for (const auto& failure : failures) if (failure) std::rethrow_exception(failure);
    std::vector<Held*> all;
    for (auto& batch : batches) for (auto& held : batch) all.push_back(&held);
    for (auto* target : all) {
        target->first->retire();
        for (auto* source : all) if (target != source)
            rejects(*target->first, role, source->second);
        require(target->first->try_reclaim().status == Reclaim::busy,
                "concurrent instance identity collision drained a live callback");
    }
    for (auto* held : all) {
        release(*held->first, role, held->second);
        require(held->first->try_reclaim().status == Reclaim::complete,
                "concurrent registry did not drain its own callback");
    }
}

int main() {
    try {
        for (const auto role : {Activity::turn, Activity::loan, Activity::callback, Activity::handoff}) {
            same_identity(role);
            same_address(role);
            concurrent_instances(role);
        }
        std::puts("PASS resource registry identity: all roles, same-address reuse, two translation units, copied/replayed tickets, 768 concurrent instances and 146688 foreign releases");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL resource registry identity: %s\n", error.what());
        return 1;
    }
}
