#include "oreslang/runtime/ActorCompactFieldStore.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <type_traits>

using oreslang::runtime::ActorCompactFieldStore;
using oreslang::runtime::CompressedActorRef;

static void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

using Store = ActorCompactFieldStore<1024, 8, 3>;
using State = Store::LinkState;

static_assert(Store::link_bytes == 4, "actor links must be 4 bytes");
static_assert(sizeof(std::array<CompressedActorRef, 3>) == 12,
              "three graph edges must consume only 12 payload bytes");
static_assert(offsetof(Store::Object, links) == 8,
              "compact links must immediately follow payload");
static_assert(std::is_trivially_copyable<Store::Object>::value,
              "compiler-backed payloads must be trivially movable");
static_assert(!std::is_copy_constructible<Store>::value,
              "actor heap cannot be cloned between actors");

static void actor_isolation_and_compact_field_roundtrip() {
    Store alice(10), bob(11);
    auto a = alice.create(101);
    auto b = alice.create(202);
    auto other = bob.create(303);
    check(a && b && other, "actor object allocation failed");
    check(!alice.link(*a, 0, *other), "foreign actor node accepted");
    check(!bob.link(*other, 0, *a), "reverse foreign actor node accepted");
    check(alice.link(*a, 0, *b), "actor-local link rejected");
    check(alice.link(*b, 0, *a), "cycle creation failed");
    check(alice.link(*a, 1, *a), "self edge rejected");
    check(bob.link(*other, 0, *other), "foreign heap self-edge fixture failed");
    check(alice.snapshot(*a)->links[1].storage()
              == bob.snapshot(*other)->links[0].storage(),
          "two actor heaps did not share the same independent local offset");

    const auto snapshot = alice.snapshot(*a);
    check(snapshot.has_value(), "object snapshots missing");
    check(snapshot->links[0].storage() != 0
              && snapshot->links[1].storage() != 0,
          "compressed reference fields were not written");
    check(alice.follow(*a, 0).state == State::Live
              && alice.follow(*a, 1).state == State::Live,
          "live compact edge lost original generation proof");
    check(alice.read_value(*alice.follow(*a, 0).target) == 202,
          "followed compact pointer resolved wrong object");
    check(alice.write_value(*b, 404) && alice.read_value(*b) == 404,
          "actor-local mutable data was not preserved");
    check(alice.unlink(*a, 1), "unlink failed");
    check(alice.follow(*a, 1).state == State::Empty,
          "empty link was not distinguished from live or stale link");
    check(!alice.link(*a, 3, *b) && !alice.unlink(*a, 3),
          "out-of-range compact field accessed");
    check(alice.follow(*a, 3).state == State::InvalidSource,
          "out-of-range field unexpectedly valid");
}

static void stale_generation_and_retirement() {
    Store store(123);
    auto source = store.create(1);
    auto target = store.create(2);
    check(source && target, "fixture allocation failed");
    check(store.link(*source, 0, *target), "fixture link failed");
    const auto stale_bits = store.snapshot(*source)->links[0].storage();
    check(store.remove(*target), "fixture remove failed");
    check(store.follow(*source, 0).state == State::Stale,
          "dead target still resolved through compact field");
    check(!store.link(*source, 0, *target),
          "dead target handle reused as original generation proof");
    auto replacement = store.create(3);
    check(replacement.has_value(), "replacement allocation failed");
    check(store.follow(*source, 0).state == State::Stale,
          "new target resurrected a stale field");
    check(store.snapshot(*source)->links[0].storage() == stale_bits,
          "stale data was silently rebound to a new target");
    check(store.link(*source, 0, *replacement), "new explicit binding failed");
    check(store.read_value(*store.follow(*source, 0).target) == 3,
          "explicit replacement did not take effect");
    check(store.remove(*source), "source removal failed");
    check(store.follow(*source, 0).state == State::InvalidSource,
          "dead source could still dereference compressed edge");
    store.retire_quiesced();
    store.retire_quiesced();
    check(store.retired() && !store.create(9), "retired actor allocated");
    check(!store.read_value(*replacement) && !store.write_value(*replacement, 9)
              && !store.remove(*replacement),
          "retired heap remained usable");
    check(store.follow(*replacement, 0).state == State::InvalidSource,
          "retired heap allowed compact access");
}

static void repeated_reuse_and_aba() {
    using Tiny = ActorCompactFieldStore<128, 2, 2>;
    Tiny actor(700);
    std::uint32_t original_offset = 0;
    for (int round = 0; round < 1200; ++round) {
        auto old = actor.create(1);
        auto partner = actor.create(2);
        check(old && partner && actor.link(*old, 0, *partner),
              "full drain graph fixture failed");
        const auto location = actor.snapshot(*old)->links[0].storage();
        if (round == 0) original_offset = location;
        check(location == original_offset,
              "actor local compressed offsets did not recycle after full drain");
        check(actor.remove(*partner), "removing target failed");
        check(actor.follow(*old, 0).state == Tiny::LinkState::Stale,
              "ABA: stale edge followed dead object");
        check(actor.remove(*old), "removing source failed");
        check(actor.follow(*old, 0).state == Tiny::LinkState::InvalidSource,
              "ABA: stale source handle followed recycled offset");
        auto fresh = actor.create(3);
        check(fresh && !actor.read_value(*old),
              "ABA: dead node proof resurrected after slot reuse");
        check(actor.remove(*fresh), "fresh removal failed");
    }

    Tiny other(701);
    auto other_node = other.create(7);
    auto a = actor.create(8);
    check(other_node && a, "cross-incarnation allocation failed");
    check(!actor.link(*a, 0, *other_node),
          "same compact offset in another heap authorized a foreign node");
    check(actor.remove(*a) && other.remove(*other_node),
          "cleanup failed");
}

int main() {
    try {
        actor_isolation_and_compact_field_roundtrip();
        stale_generation_and_retirement();
        repeated_reuse_and_aba();
        std::cout << "PASS compact32 native actor fields, cycle links, foreign proof, "
                     "stale generation, retirement and recycled offsets\\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL compact32 actor fields: " << e.what() << '\n';
        return 1;
    }
}
