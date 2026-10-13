#include "oreslang/runtime/EpochArena.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

using oreslang::runtime::EpochArena;

static void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class Fn>
static void must_throw(Fn&& fn, const char* message) {
    bool threw = false;
    try { fn(); } catch (const std::exception&) { threw = true; }
    check(threw, message);
}

struct Resource {
    std::vector<int>* drops;
    int id;
    Resource(std::vector<int>& result, int value) noexcept
        : drops(&result), id(value) {}
    ~Resource() noexcept { drops->push_back(id); }
};
struct ThrowsOnCreate {
    explicit ThrowsOnCreate(bool fail) {
        if (fail) throw std::runtime_error("bad constructor");
    }
    ~ThrowsOnCreate() noexcept = default;
};
struct Overaligned {
    alignas(128) char bytes[128];
    ~Overaligned() noexcept = default;
};

static_assert(!std::is_copy_constructible<EpochArena<1024, 4>::Ref<Resource>>::value,
              "references must be move-only");

static void test_reverse_destructors_and_epoch_reuse() {
    EpochArena<1024, 4> arena;
    std::vector<int> drops;
    auto first = arena.emplace<Resource>(drops, 1);
    auto second = arena.emplace<Resource>(drops, 2);
    first.with([](Resource& value) { check(value.id == 1, "first value corrupted"); });
    check(arena.live_objects() == 2 && arena.live_refs() == 2, "bad arena accounting");
    const std::size_t before = arena.used();
    const auto gen = arena.generation();
    must_throw([&] { arena.reset(); }, "actor epoch reset with live ref succeeded");
    check(arena.used() == before, "failed epoch reset changed allocator cursor");
    first.reset();
    second.reset();
    check(drops.empty(), "arena unexpectedly drops before epoch reset");
    arena.reset();
    check(arena.used() == 0 && arena.live_objects() == 0 && arena.live_refs() == 0,
          "epoch reset didn't reclaim the arena");
    check(drops == std::vector<int>({2, 1}), "reverse-construction drop order violated");
    check(arena.generation() == gen + 1, "epoch ID did not advance");
    auto next = arena.emplace<Resource>(drops, 3);
    next.reset();
    arena.close();
    check(drops == std::vector<int>({2, 1, 3}), "close missed destructor");
    must_throw([&] { (void)arena.emplace<Resource>(drops, 4); },
               "closed actor arena admitted allocation");
}

static void test_exception_and_capacity() {
    EpochArena<128, 2> arena;
    const auto before = arena.used();
    must_throw([&] { (void)arena.emplace<ThrowsOnCreate>(true); },
               "throwing constructor did not propagate");
    check(arena.used() == before && arena.live_objects() == 0,
          "failed construction changed arena accounting");
    auto good = arena.emplace<ThrowsOnCreate>(false);
    auto good2 = arena.emplace<ThrowsOnCreate>(false);
    must_throw([&] { (void)arena.emplace<ThrowsOnCreate>(false); },
               "object slot cap was not enforced");
    good.reset();
    good2.reset();
    arena.reset();
    check(arena.used() == 0, "slot limit rejection corrupted cursor");

    EpochArena<8, 8> tiny;
    auto one = tiny.emplace<std::uint64_t>(5);
    must_throw([&] { (void)tiny.emplace<std::uint64_t>(6); },
               "byte capacity limit was not enforced");
    one.with([](std::uint64_t& value) { check(value == 5, "bounded allocation corrupt"); });
    one.reset();
    tiny.close();
}

static void test_borrow_transfer_and_delayed_reset() {
    std::vector<int> drops;
    EpochArena<1024, 8> arena;
    auto pinned = arena.emplace<Resource>(drops, 1);
    auto moved = std::move(pinned);
    check(!pinned && moved, "epoch move should transfer exactly one lease");
    must_throw([&] { pinned.with([](Resource&) {}); },
               "moved epoch reference still usable");
    must_throw([&] { arena.reset(); }, "moved lease not counted as live");
    moved.reset();
    arena.reset();
    check(drops == std::vector<int>({1}), "moved ref resulted in double drop");
}

static void test_auto_drop_at_arena_destruction() {
    std::vector<int> drops;
    {
        EpochArena<1024, 4> arena;
        auto a = arena.emplace<Resource>(drops, 10);
        auto b = arena.emplace<Resource>(drops, 11);
        b.reset();
        a.reset();
        // No explicit close needed when compiler has discharged all leases.
    }
    check(drops == std::vector<int>({11, 10}),
          "actor arena destruction missed resources");
}

int main() {
    test_reverse_destructors_and_epoch_reuse();
    test_exception_and_capacity();
    test_borrow_transfer_and_delayed_reset();
    test_auto_drop_at_arena_destruction();
    std::cout << "PASS: actor region quiescence, reverse deterministic drops, "
                 "generation reuse, capacity, partial construction and lease moves\n";
}
