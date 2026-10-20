#include "oreslang/runtime/ActorManagedSlotArena.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>

using namespace oreslang::runtime;

static void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class Fn>
static void rejects(Fn&& fn, const char* message) {
    bool rejected = false;
    try { fn(); } catch (const std::logic_error&) { rejected = true; }
    check(rejected, message);
}

template<ManagedRefMode Mode>
static void test_actor_local_arena() {
    using Arena = ActorManagedSlotArena<3, 17, Mode>;
    using Ref = typename Arena::Ref;
    static_assert(Arena::slot_stride() == 24, "slot alignment changed");
    static_assert(Arena::arena_bytes() == 72, "slot layout changed");
    static_assert(sizeof(Ref) == (Mode == ManagedRefMode::Compressed32 ? 4u : 8u),
                  "incorrect physical reference width");

    const ActorHeapIdentity alice{10, 11, 1}, bob{10, 12, 1};
    Arena a(alice), b(bob);
    std::array<std::byte, 3> data{std::byte{42}, std::byte{21}, std::byte{7}};
    const auto first = a.allocate(alice, data.data(), data.size());
    const auto second = a.allocate(alice, data.data(), data.size());
    const auto third = a.allocate(alice, data.data(), data.size());
    check(first && second && third, "arena did not allocate its fixed capacity");
    check(!a.allocate(alice, data.data(), data.size()), "arena exceeded slot quota");
    check(first->arena == a.arena_id(), "handle's domain differs from owner arena");
    check(first->reference.storage() != second->reference.storage(),
          "distinct objects have identical compact references");
    if constexpr (Mode == ManagedRefMode::Compressed32) {
        check(first->reference.storage() == 1 && second->reference.storage() == 4 &&
              third->reference.storage() == 7,
              "compressed slot offset encoding is not region-relative");
    }
    std::array<std::byte, 17> output{};
    check(a.copy_out(alice, *first, output.data(), output.size()) == 3,
          "arena returned incorrect payload length");
    check(output[0] == data[0] && output[2] == data[2],
          "arena payload content was not preserved");

    // An external 64-bit FFI pointer is never a stored 4-byte reference.
    const auto foreign = b.allocate(bob, data.data(), data.size());
    check(foreign && foreign->arena != first->arena, "arena identity collision");
    if constexpr (Mode == ManagedRefMode::Compressed32)
        check(foreign->reference.storage() == first->reference.storage(),
              "test did not exercise colliding actor-relative offset bits");
    rejects([&] { (void)a.copy_out(bob, *first, output.data(), output.size()); },
            "foreign actor read owner arena");
    rejects([&] { (void)a.copy_out(alice, *foreign, output.data(), output.size()); },
            "foreign compact reference used as owner arena handle");
    rejects([&] { (void)a.copy_out(alice,
        typename Arena::Handle{a.arena_id(), Ref{}, first->generation},
        output.data(), output.size()); }, "null compact ref resolved to actor bytes");
    if constexpr (Mode == ManagedRefMode::Compressed32) {
        // A numerically in-range offset that points inside an allocation is
        // not a valid managed object start even when perfectly 8-byte aligned.
        const auto interior = Ref::encode(0x1000, 0x100, 0x1008);
        check(interior.has_value(), "could not construct the interior-ref probe");
        rejects([&] { (void)a.copy_out(alice,
            typename Arena::Handle{a.arena_id(), *interior, first->generation},
            output.data(), output.size()); }, "interior ref bypassed allocation-start validation");
    }

    bool bad_input = false;
    try { (void)a.allocate(alice, nullptr, 1); }
    catch (const std::invalid_argument&) { bad_input = true; }
    check(bad_input, "null input accepted");

    bad_input = false;
    try { (void)a.copy_out(alice, *first, output.data(), 2); }
    catch (const std::invalid_argument&) { bad_input = true; }
    check(bad_input, "undersized output buffer accepted");

    a.release(alice, *first);
    rejects([&] { (void)a.copy_out(alice, *first, output.data(), output.size()); },
            "stale generation revived after release");
    const auto reused = a.allocate(alice, data.data(), data.size());
    check(reused && reused->reference == first->reference &&
          reused->generation == first->generation + 1,
          "slot reuse did not fence stale references");
    rejects([&] { a.release(alice, *first); }, "stale release freed reallocated slot");

    auto lease = a.borrow(alice);
    check(lease && static_cast<bool>(*lease), "actor lifetime lease denied");
    bool foreign_borrow_denied = false;
    try { (void)a.borrow(bob); } catch (const std::logic_error&) {
        foreign_borrow_denied = true;
    }
    check(foreign_borrow_denied, "foreign actor obtained an FFI/lifetime lease");
    lease->reset();
}

static void test_domain_id_saturation() {
    std::atomic<std::uint64_t> counter{std::numeric_limits<std::uint64_t>::max() - 1};
    check(detail::reserve_managed_slot_arena_id(counter) ==
          std::numeric_limits<std::uint64_t>::max() - 1,
          "domain counter did not issue penultimate ID");
    bool overflow = false;
    try { (void)detail::reserve_managed_slot_arena_id(counter); }
    catch (const std::overflow_error&) { overflow = true; }
    check(overflow && counter == std::numeric_limits<std::uint64_t>::max(),
          "domain counter wrapped instead of saturating");
}

int main() {
    test_domain_id_saturation();
    test_actor_local_arena<ManagedRefMode::Compressed32>();
    test_actor_local_arena<ManagedRefMode::Native64>();
}
