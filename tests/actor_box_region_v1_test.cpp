#include "oreslang/runtime/ActorBoxRegionV1.hpp"

#include <atomic>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <type_traits>

using oreslang::runtime::ActorHeapIdentity;
using oreslang::runtime::native_ownership_v1::ActorBoxRegion;

#define REQUIRE(v) do { if (!(v)) throw std::runtime_error("failed: " #v); } while (false)

struct Probe {
    int value;
    std::atomic<int>* drops;
    Probe(int init, std::atomic<int>* counter) noexcept : value(init), drops(counter) {}
    ~Probe() noexcept { drops->fetch_add(1, std::memory_order_relaxed); }
};
static_assert(!std::is_copy_constructible_v<ActorBoxRegion<Probe, 2>>);
static_assert(!std::is_move_constructible_v<ActorBoxRegion<Probe, 2>>);

int main() {
    const ActorHeapIdentity alice{11, 22, 33};
    const ActorHeapIdentity alice_new_epoch{11, 22, 34};
    const ActorHeapIdentity bob{11, 23, 33};
    std::atomic<int> drops{0};
    ActorBoxRegion<Probe, 2> arena(alice);

    REQUIRE(!arena.emplace_at(alice, 0, 5, &drops));
    REQUIRE(!arena.bind_carrier(bob));
    REQUIRE(!arena.bind_carrier(alice_new_epoch));
    REQUIRE(arena.bind_carrier(alice));
    REQUIRE(arena.arena_domain() != 0);
    REQUIRE(arena.emplace_at(alice, 0, 5, &drops));
    REQUIRE(!arena.emplace_at(alice, 0, 99, &drops));
    REQUIRE(!arena.emplace_at(bob, 1, 5, &drops));
    REQUIRE(!arena.emplace_at(alice_new_epoch, 1, 5, &drops));
    REQUIRE(!arena.retire_drained(alice));

    int seen = 0;
    REQUIRE(arena.with_at(alice, 0, [&](Probe& value) {
        seen = value.value;
        value.value += 7;
        REQUIRE(!arena.drop_at(alice, 0));
        REQUIRE(!arena.retire_drained(alice));
        REQUIRE(!arena.emplace_at(alice, 1, 4, &drops));
        REQUIRE(!arena.with_at(alice, 0, [](Probe&) {}));
    }));
    REQUIRE(seen == 5);
    REQUIRE(arena.with_at(alice, 0, [&](Probe& value) {
        REQUIRE(value.value == 12);
    }));
    REQUIRE(!arena.with_at(bob, 0, [](Probe&) {}));

    std::atomic<bool> foreign_denied{false};
    std::thread foreign([&] {
        const bool denied =
            !arena.bind_carrier(alice) &&
            !arena.emplace_at(alice, 1, 3, &drops) &&
            !arena.with_at(alice, 0, [](Probe&) {}) &&
            !arena.drop_at(alice, 0) &&
            !arena.retire_drained(alice);
        foreign_denied.store(denied, std::memory_order_release);
    });
    foreign.join();
    REQUIRE(foreign_denied.load(std::memory_order_acquire));
    REQUIRE(drops.load() == 0);

    // Callback failure must release the transient loan.
    bool threw = false;
    try {
        arena.with_at(alice, 0, [](Probe&) { throw std::runtime_error("abort callback"); });
    } catch (const std::runtime_error&) { threw = true; }
    REQUIRE(threw);
    REQUIRE(arena.drop_at(alice, 0));
    REQUIRE(drops.load() == 1);
    REQUIRE(!arena.with_at(alice, 0, [](Probe&) {}));
    REQUIRE(!arena.drop_at(alice, 0));
    REQUIRE(arena.emplace_at(alice, 0, 99, &drops)); // new slot generation
    REQUIRE(arena.drop_at(alice, 0));
    REQUIRE(drops.load() == 2);
    REQUIRE(arena.retire_drained(alice));
    REQUIRE(arena.retire_drained(alice));
    REQUIRE(!arena.emplace_at(alice, 0, 3, &drops));
    std::puts("ATTO_ACTOR_BOX_REGION_V1_PASS");
}
