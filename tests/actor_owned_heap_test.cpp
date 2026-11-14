#include "oreslang/runtime/ActorOwnedHeap.hpp"
#include "oreslang/runtime/ActorHeapScope.hpp"
#include <array>
#include <atomic>
#include <limits>
#include <cstdint>
#include <iostream>
#include <optional>
#include <new>
#include <stdexcept>
#include <type_traits>

using namespace oreslang::runtime;

static void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

using Heap = ActorOwnedHeap<512, 8>;
using Handle = Heap::Handle;
struct Node {
    Handle next{};
    std::int64_t value = 0;
};
static_assert(std::is_trivially_copyable<Node>::value, "node is POD-like");
static_assert(!std::is_copy_constructible<Heap>::value, "heap cannot be cloned");
static_assert(!std::is_move_constructible<Heap>::value, "heap cannot be moved");

static void test_independent_actor_storage_and_cycles() {
    const ActorHeapIdentity alice_id{77, 0, 11};
    const ActorHeapIdentity bob_id{77, 1, 12};
    Heap alice(alice_id);
    Heap bob(bob_id);
    check(alice.allocation_domain_bound(), "actor heap must bind allocation authority");
    check(same_actor_allocation_domain(alice.allocation_domain(), alice_id),
          "actor heap capability must preserve runtime/slot/generation");
    check(alice.allocation_domain().owner == 0,
          "actor slot zero is a valid allocation owner");
    check(bob.allocation_domain().owner == 1,
          "distinct actor heap capability must remain distinct");
    auto one = alice.allocate(sizeof(Node), alignof(Node));
    auto two = alice.allocate(sizeof(Node), alignof(Node));
    check(one && two, "two actor-owned nodes must allocate");
    check(alice.write(*one, Node{*two, 11}), "first edge must write");
    check(alice.write(*two, Node{*one, 22}), "reciprocal cycle must write");
    auto first = alice.read<Node>(*one);
    auto second = alice.read<Node>(*two);
    check(first && second, "cyclic arena graph must resolve");
    check(first->next == *two && second->next == *one,
          "cycle IDs must survive roundtrip");
    check(!bob.read<Node>(*one), "cross-actor heap handle must fail closed");
    check(!bob.write(*two, Node{*one, 33}),
          "cross-actor write must not mutate foreign heap");
    check(bob.used() == 0, "cross-actor attack must not allocate");
    check(alice.live_allocations() == 2, "actor allocations tracked separately");
    check(alice.remove(*two), "remove second actor node");
    check(!alice.read<Node>(*two), "removed node handle must stay stale");
    check(!alice.remove(*two), "double-remove must fail");
    auto replacement = alice.allocate(sizeof(Node), alignof(Node));
    check(replacement.has_value(), "reused slot must allocate");
    check(!alice.read<Node>(*two), "reused slot must not resurrect old ID");
    check(alice.write(*replacement, Node{*replacement, 42}),
          "self-cycle must be supported without reference counting");
    check(alice.read<Node>(*replacement)->next == *replacement,
          "self-cycle handle roundtrip");
    check(alice.live_allocations() == 2, "removed slot capacity reclaimed");

    alice.retire_quiesced();
    alice.retire_quiesced();
    check(alice.retired() && alice.live_allocations() == 0,
          "retired heap must be cleared");
    check(!alice.read<Node>(*one) && !alice.read<Node>(*replacement),
          "no prior actor handle may resolve after retirement");
    check(!alice.allocate(8), "retired heap cannot allocate");
    check(bob.allocate(8).has_value(),
          "retiring one actor cannot retire a different actor heap");
}

static void test_bounded_storage_and_invalid_accesses() {
    using Small = ActorOwnedHeap<64, 2>;
    Small heap(7);
    check(!heap.allocate(0), "zero allocation must reject");
    check(!heap.allocate(8, 3), "non-power-of-two alignment must reject");
    check(!heap.allocate(1024), "byte quota must reject");
    check(heap.used() == 0, "failed allocation must leave accounting unchanged");
    auto a = heap.allocate(16, 8);
    auto b = heap.allocate(16, 8);
    check(a && b, "slots must allocate");
    check(!heap.allocate(1), "slot quota must reject");
    check(heap.write(*a, std::uint64_t{123}, 8), "in-bounds write");
    check(!heap.write(*a, std::uint64_t{123}, 10), "out-of-bounds write");
    check(heap.read<std::uint64_t>(*a, 8).value() == 123,
          "in-bounds read");
    check(!heap.read<std::uint64_t>(*a, 10), "out-of-bounds read");
    check(heap.remove(*a), "remove slot");
    auto reused = heap.allocate(8, 8);
    check(reused && !heap.read<std::uint64_t>(*a),
          "slot generation must invalidate stale aliases");
}

// Reconstructing a heap at exactly the same address must not resurrect an
// old actor's handles. This also guards hot reload and actor-ID reuse.
static void test_long_lived_actor_full_drain_recycles_bytes_without_aba() {
    using Tiny = ActorOwnedHeap<64, 2>;
    Tiny heap(9);
    auto old = heap.allocate(16, 8);
    check(old.has_value(), "initial allocation");
    check(heap.remove(*old), "initial deterministic free");
    check(heap.used() == 0, "last free must recycle byte capacity");

    // Without a full-drain reset, a bump allocator runs out after only
    // a handful of request handlers even though it has no live objects.
    for (int n = 0; n < 1000; ++n) {
        auto current = heap.allocate(16, 8);
        check(current.has_value(), "long-lived actor must reuse freed bytes");
        check(!heap.read<std::uint64_t>(*old),
              "stale generation must never resurrect after cursor reset");
        check(heap.read<std::uint64_t>(*current).value() == 0,
              "reused memory must be zeroed");
        check(heap.write(*current, std::uint64_t{123}),
              "recycled allocation remains writable");
        check(heap.remove(*current), "deterministic removal");
        check(heap.used() == 0 && heap.live_allocations() == 0,
              "every completed request releases bump capacity");
    }
}

// A persistent actor field must not make a zero-GC heap exhaust merely
// because many ephemeral messages allocate/drop their *last* native object.
// This verifies alignment padding is rewound along with the allocation and
// that the persistent allocation is never moved or invalidated.
static void test_transient_bump_tail_reuse_with_persistent_fields() {
    using Tiny = ActorOwnedHeap<96, 4>;
    Tiny heap(97);
    const auto persistent = heap.allocate(16, 8);
    check(persistent.has_value(), "persistent field allocation");
    check(heap.write(*persistent, std::uint64_t{0xA1B2C3D4}),
          "persistent value initialized");
    const std::size_t committed = heap.used();

    std::optional<Tiny::Handle> retired;
    for (std::size_t n = 0; n < 4096; ++n) {
        const std::size_t alignment = (n & 1) == 0
            ? alignof(std::max_align_t) : alignof(std::uint32_t);
        const auto temporary = heap.allocate(40, alignment);
        check(temporary.has_value(), "short-lived actor allocation exhausted");
        check(heap.read<std::uint64_t>(*temporary).value() == 0,
              "reused temporary allocation must be zero-filled");
        check(heap.write(*temporary, std::uint64_t{n}),
              "temporary payload write failed");
        if (retired) check(!heap.read<std::uint64_t>(*retired),
                           "stale temporary handle resurrected after reuse");
        check(heap.remove(*temporary), "temporary native destructor/drop");
        check(heap.used() == committed,
              "transient deletion did not restore pre-alignment bump cursor");
        check(heap.live_allocations() == 1,
              "persistent actor field must remain live through scratch churn");
        check(heap.read<std::uint64_t>(*persistent).value() == 0xA1B2C3D4,
              "persistent actor field was moved or mutated");
        retired = temporary;
    }
    check(heap.remove(*persistent), "persistent field released");
    check(heap.used() == 0 && heap.live_allocations() == 0,
          "actor heap not drained after all fields released");

    // A freed interior allocation cannot rewind across a later live object.
    Tiny partial(98);
    auto first = partial.allocate(16, 8);
    auto second = partial.allocate(16, 8);
    auto third = partial.allocate(16, 8);
    check(first && second && third, "three native allocations");
    auto before = partial.used();
    check(partial.remove(*second), "interior drop");
    check(partial.used() == before,
          "interior free rewound over still-live tail allocation");
    check(partial.remove(*third), "tail drop");
    check(partial.used() >= 16,
          "tail free invalidated first still-live allocation");
    check(partial.read<std::uint64_t>(*first).has_value(),
          "interior/tail free invalidated persistent handle");
}

static void test_coalesced_interior_holes_preserve_live_actor_payloads() {
    using Tiny = ActorOwnedHeap<128, 8>;
    Tiny heap(104);
    const auto anchor = heap.allocate(16, 8);
    const auto middle1 = heap.allocate(16, 8);
    const auto middle2 = heap.allocate(16, 8);
    const auto tail = heap.allocate(16, 8);
    check(anchor && middle1 && middle2 && tail, "bounded hole setup");
    check(heap.write(*anchor, std::uint64_t{0x1122334455667788ULL}),
          "persistent actor payload write");
    check(heap.write(*tail, std::uint64_t{0xAABBCCDDEEFF0011ULL}),
          "tail actor payload write");
    const auto used_before = heap.used();

    check(heap.remove(*middle1), "free first middle object");
    check(heap.remove(*middle2), "free second middle object");
    check(heap.used() == used_before, "interior frees must not move bump tail");
    check(!heap.read<std::uint64_t>(*middle1)
          && !heap.read<std::uint64_t>(*middle2),
          "generations retired before hole reclamation");

    // A 32-byte request must fit the two coalesced 16-byte holes. It must
    // not consume new bump bytes or relocate either live object.
    const auto replacement = heap.allocate(32, 8);
    check(replacement.has_value(), "neighboring holes failed to coalesce");
    check(heap.used() == used_before, "coalesced allocation grew actor bump");
    check(heap.read<std::uint64_t>(*replacement).value() == 0,
          "reused interior extent must be zero-filled");
    check(heap.read<std::uint64_t>(*anchor).value() == 0x1122334455667788ULL,
          "persistent field changed after hole reuse");
    check(heap.read<std::uint64_t>(*tail).value() == 0xAABBCCDDEEFF0011ULL,
          "live actor tail moved during interior reuse");
    check(heap.remove(*replacement), "drop reused interior extent");
    check(heap.remove(*tail), "release bump tail");
    check(heap.used() == 16,
          "tail rewind must consume adjacent coalesced hole");
    check(heap.remove(*anchor), "final persistent field reclamation");
    check(heap.used() == 0, "final drain failed to recycle actor heap");
}

static void test_randomized_interior_reclamation_preserves_stale_fences() {
    using Tiny = ActorOwnedHeap<512, 8>;
    Tiny heap(105);
    std::array<std::optional<Tiny::Handle>, 8> active{};
    std::array<std::uint64_t, 8> values{};
    std::optional<Tiny::Handle> stale{};
    std::uint32_t state = 0xBEEFCAFEu;
    auto random = [&]() noexcept {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    };
    for (int step = 0; step < 50000; ++step) {
        const std::size_t choice = random() % active.size();
        if (active[choice]) {
            stale = *active[choice];
            check(heap.remove(*active[choice]), "seeded drop rejected valid handle");
            active[choice].reset();
        } else {
            const std::size_t width = 8 + (random() % 40);
            const std::size_t alignment = std::size_t{1} << (random() % 5);
            const auto h = heap.allocate(width, alignment);
            if (h) {
                values[choice] = std::uint64_t(random()) + 1;
                check(heap.write(*h, values[choice]), "seeded actor payload write");
                active[choice] = h;
            }
        }
        std::size_t alive = 0;
        for (std::size_t i = 0; i < active.size(); ++i) {
            if (!active[i]) continue;
            ++alive;
            const auto value = heap.read<std::uint64_t>(*active[i]);
            check(value && *value == values[i],
                  "interior reclamation overlapped or corrupted a live actor");
        }
        check(heap.live_allocations() == alive,
              "model/live actor slot accounting mismatch");
        check(heap.used() <= Tiny::capacity, "actor byte budget overflowed");
        if (stale) check(!heap.read<std::uint64_t>(*stale),
                         "previously freed generational identity revived");
    }
}

static void test_heap_address_reuse_aba() {
    alignas(Heap) unsigned char storage[sizeof(Heap)];
    Heap* former = new (storage) Heap(33);
    auto old = former->allocate(8);
    check(old.has_value(), "old heap allocation");
    former->~Heap();
    Heap* fresh = new (storage) Heap(33);
    auto value = fresh->allocate(8);
    check(value.has_value(), "new actor incarnation must allocate");
    check(!fresh->read<std::uint64_t>(*old),
          "new actor heap must reject old-generation handle");
    fresh->~Heap();
}

// Actor fields can store compact locations (4 bytes per edge), while the
// trusted runtime keeps the original full-domain/generation proof off-line.
static void test_compact_actor_local_refs() {
    using Local = ActorOwnedHeap<128, 4>;
    static_assert(sizeof(Local::CompactRef) == 4, "compact pointer width");
    static_assert(sizeof(std::array<Local::CompactRef, 2>) == 8, "compact edges");
    static_assert(sizeof(std::array<NativeActorManagedRef, 2>) == 16, "native edges");
    Local alice(501), bob(502);
    const auto a = alice.allocate(16, 8);
    const auto b = bob.allocate(16, 8);
    check(a && b, "two actor heaps allocate");
    const auto ar = alice.compact(*a);
    const auto br = bob.compact(*b);
    check(ar && br, "compact encoding admitted live aligned objects");
    check(ar->storage() == br->storage(),
          "independent heaps should permit the same local compressed offset");
    check(alice.compact_matches(*a, *ar), "own compact reference must match");
    check(bob.compact_matches(*b, *br), "second own compact reference must match");
    check(!alice.compact_matches(*b, *ar) && !bob.compact_matches(*a, *br),
          "foreign actor proof must not authorize a compact reference");
    check(!alice.compact_matches(*a, Local::CompactRef{}),
          "null compact reference is not an object");
    std::array<Local::CompactRef, 2> edges{*ar, *ar};
    check(alice.write(*a, edges), "two compact edges fit in a 16-byte allocation");
    const auto roundtrip = alice.read<decltype(edges)>(*a);
    check(roundtrip && (*roundtrip)[0] == *ar && (*roundtrip)[1] == *ar,
          "stored compact edges must survive roundtrip");
    check(alice.remove(*a), "initial allocation retires");
    check(!alice.compact(*a) && !alice.compact_matches(*a, *ar),
          "stale generation must fail even when the raw offset remains");
    const auto fresh = alice.allocate(16, 8);
    check(fresh && alice.compact(*fresh) && !(*fresh == *a),
          "new generation must allocate successfully");
    // A location alone cannot prevent ABA: only the ORIGINAL handle/lifetime
    // proof may validate it. A fresh proof must never be paired with a stale edge.
    check(alice.compact(*fresh)->storage() == ar->storage(),
          "recycled address should expose why bare compact refs are not capabilities");
    alice.retire_quiesced();
    check(!alice.compact(*fresh) && !alice.compact_matches(*fresh, *ar),
          "retirement must revoke compressed views");

    Local misaligned(503);
    const auto first = misaligned.allocate(1, 1);
    const auto second = misaligned.allocate(1, 1);
    check(first && second, "misaligned allocation fixture");
    check(misaligned.compact(*first).has_value(),
          "heap-base-aligned allocation is encodable");
    check(!misaligned.compact(*second),
          "sub-8-byte-aligned object must not silently truncate its offset");
}

static void test_heap_identity_terminal_exhaustion() {
    // Regression: fetch_add previously wrapped the counter from UINT64_MAX
    // to zero and eventually issued identity 1 again after two exceptions.
    std::atomic<std::uint64_t> nearly_exhausted{
        std::numeric_limits<std::uint64_t>::max() - 1};
    const auto last = detail::reserve_actor_owned_heap_id(nearly_exhausted);
    check(last == std::numeric_limits<std::uint64_t>::max() - 1,
          "last actor heap identity must be unique");
    check(nearly_exhausted.load() == std::numeric_limits<std::uint64_t>::max(),
          "counter must stay terminal at exhaustion");

    for (int i = 0; i < 8; ++i) {
        bool rejected = false;
        try {
            (void)detail::reserve_actor_owned_heap_id(nearly_exhausted);
        } catch (const std::overflow_error&) {
            rejected = true;
        }
        check(rejected && nearly_exhausted.load() ==
                std::numeric_limits<std::uint64_t>::max(),
              "identity exhaustion must fail closed without counter wrap");
    }

    std::atomic<std::uint64_t> invalid_zero{0};
    bool rejected = false;
    try { (void)detail::reserve_actor_owned_heap_id(invalid_zero); }
    catch (const std::overflow_error&) { rejected = true; }
    check(rejected && invalid_zero.load() == 0,
          "invalid terminal identity zero must never be issued");
}

static void test_allocation_scope_auto_cleanup_and_transfer() {
    using ScopedHeap = ActorOwnedHeap<256, 8>;
    ScopedHeap heap(ActorHeapIdentity{900, 3, 1});

    ScopedHeap::Handle transferred{};
    {
        ActorHeapScope<ScopedHeap, 4> scope(heap);
        auto first = scope.allocate(24, 8);
        auto second = scope.allocate(24, 8);
        check(first && second, "scope allocations must succeed");
        check(scope.tracked() == 2 && heap.live_allocations() == 2,
              "scope tracks each lexical owner");

        check(scope.drop(*first), "explicit lexical drop succeeds");
        check(scope.tracked() == 1 && heap.live_allocations() == 1,
              "explicit drop disarms exactly one cleanup");

        transferred = *second;
        check(scope.release(*second), "move/transfer disarms lexical cleanup");
        check(scope.tracked() == 0 && heap.live_allocations() == 1,
              "released owner remains live for its new owner");
    }
    check(heap.live_allocations() == 1,
          "scope destructor must not free transferred ownership");
    check(heap.remove(transferred), "new owner performs final deterministic drop");
    check(heap.live_allocations() == 0 && heap.used() == 0,
          "last deterministic drop recycles the bump region");

    {
        ActorHeapScope<ScopedHeap, 4> scope(heap);
        auto a = scope.allocate(16, 8);
        auto b = scope.allocate(16, 8);
        auto c = scope.allocate(16, 8);
        check(a && b && c, "three lexical owners allocate");
        check(heap.live_allocations() == 3, "three owners are live");
    }
    check(heap.live_allocations() == 0 && heap.used() == 0,
          "scope destructor drops every untransferred owner");

    ActorHeapScope<ScopedHeap, 1> bounded(heap);
    check(bounded.allocate(8).has_value(), "bounded scope first allocation");
    check(!bounded.allocate(8), "scope capacity must fail closed");
    check(bounded.close(), "explicit close succeeds");
    check(bounded.close(), "close is idempotent");
}

static void test_scope_detects_out_of_band_invalidation_without_leaking_rest() {
    using ScopedHeap = ActorOwnedHeap<256, 8>;
    ScopedHeap heap(ActorHeapIdentity{901, 4, 1});
    ActorHeapScope<ScopedHeap, 4> scope(heap);
    auto first = scope.allocate(16, 8);
    auto second = scope.allocate(16, 8);
    check(first && second, "scope fixture allocations");
    check(heap.remove(*first), "out-of-band drop fixture");
    check(!scope.close(), "scope reports violated cleanup ownership");
    check(heap.live_allocations() == 0,
          "one stale tracked handle cannot suppress cleanup of remaining owners");
}

int main() {
    try {
        test_heap_identity_terminal_exhaustion();
        test_independent_actor_storage_and_cycles();
        test_compact_actor_local_refs();
        test_bounded_storage_and_invalid_accesses();
        test_heap_address_reuse_aba();
        test_long_lived_actor_full_drain_recycles_bytes_without_aba();
        test_transient_bump_tail_reuse_with_persistent_fields();
        test_coalesced_interior_holes_preserve_live_actor_payloads();
        test_randomized_interior_reclamation_preserves_stale_fences();
        test_allocation_scope_auto_cleanup_and_transfer();
        test_scope_detects_out_of_band_invalidation_without_leaking_rest();
        std::cout << "PASS native actor-owned byte heap contracts\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << "FAIL: " << failure.what() << '\n';
        return 1;
    }
}
