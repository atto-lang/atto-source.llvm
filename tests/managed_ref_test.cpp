#include "oreslang/runtime/ManagedRef.hpp"
#include "oreslang/runtime/DopLayout.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace {

struct EntityTag {};
struct ComponentTag {};

} // namespace

int main() {
    using namespace oreslang::runtime;

    static_assert(sizeof(CompressedActorRef) == 4);
    static_assert(sizeof(NativeActorManagedRef) == 8);
#if defined(ORES_MANAGED_REF_DEFAULT_NATIVE64)
    static_assert(sizeof(DefaultActorManagedRef) == 8);
#else
    static_assert(sizeof(DefaultActorManagedRef) == 4);
#endif

    alignas(8) std::array<std::byte, 256> arena{};
    const auto base = reinterpret_cast<std::uintptr_t>(arena.data());

    auto first = CompressedActorRef::encode(base, arena.size(), base);
    auto third = CompressedActorRef::encode(base, arena.size(), base + 16);
    assert(first && third);
    assert(first->storage() == 1);
    assert(third->storage() == 3);
    assert(first->decode(base, arena.size()).value() == base);
    assert(third->decode(base, arena.size()).value() == base + 16);

    auto null_ref = CompressedActorRef::encode(base, arena.size(), 0);
    assert(null_ref && null_ref->is_null());
    assert(null_ref->decode(base, arena.size()).has_value());
    assert(null_ref->decode(base, arena.size()).value() == 0);

    assert(!CompressedActorRef::encode(base, arena.size(), base + 3));
    assert(!CompressedActorRef::encode(base, arena.size(), base + arena.size()));
    assert(!third->decode(base, 8));

    auto native = NativeActorManagedRef::encode(base, arena.size(), base + 24);
    assert(native);
    assert(native->storage() == static_cast<std::uint64_t>(base + 24));
    assert(native->decode(base, arena.size()).value() == base + 24);
    assert(!native->decode(base + 32, arena.size() - 32));

    auto entity = DenseIndex32<EntityTag>::from_index(41);
    auto component = DenseIndex32<ComponentTag>::from_index(41);
    assert(entity && component);
    assert(entity->index().value() == 41);
    assert(component->index().value() == 41);
    static_assert(sizeof(DenseIndex32<EntityTag>) == 4);
    static_assert(sizeof(DenseIndex32<ComponentTag>) == 4);
    assert(!DenseIndex32<EntityTag>::from_index(
        static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())));

    // DOD contract: compact refs/indices can be laid out as 4-byte scalar
    // columns or AoSoA lanes without changing their logical source-level type.
    oreslang::dop::Plan soa(oreslang::dop::Layout::SoA, 1024, 3, 4, 16, 1);
    oreslang::dop::Plan aosoa(oreslang::dop::Layout::AoSoA, 1024, 3, 4, 16, 8);
    assert(soa.byte_offset(1, 0) == 4);
    assert(aosoa.byte_offset(1, 0) == 4);
    assert(aosoa.byte_offset(0, 1) == 32);

    static_assert(CompressedActorRef::max_addressable_span ==
                  UINT64_C(34359738360)); // 32 GiB - 8 at 8-byte alignment

    // The full 32-bit range is real even though no enormous actor arena is
    // allocated for this boundary test. High-bit words must be treated as
    // unsigned stored data, never as signed Java/LLVM guest integers.
    if (sizeof(std::uintptr_t) > sizeof(std::uint32_t)) {
        const auto synthetic_base = static_cast<std::uintptr_t>(UINT64_C(0x1000000000));
        const auto span = static_cast<std::size_t>(
            CompressedActorRef::max_addressable_span);
        const auto last_offset = static_cast<std::uintptr_t>(
            CompressedActorRef::max_offset);
        auto last = CompressedActorRef::encode(
            synthetic_base, span, synthetic_base + last_offset);
        assert(last && last->storage() == UINT32_MAX);
        assert(last->decode(synthetic_base, span) == synthetic_base + last_offset);

        const auto high_bit_offset = static_cast<std::uintptr_t>(UINT64_C(1) << 34);
        auto high = CompressedActorRef::encode(
            synthetic_base, span, synthetic_base + high_bit_offset);
        assert(high && high->storage() > static_cast<std::uint32_t>(INT32_MAX));
        assert(high->decode(synthetic_base, span) ==
               synthetic_base + high_bit_offset);

        // Compression must not partially accept a region whose declared
        // extent cannot be encoded. The allocator must select native64 first.
        assert(!CompressedActorRef::encode(synthetic_base, span + 8, synthetic_base));
        assert(!first->decode(synthetic_base, span + 8));
    }

    // Reject malformed regions crossing the machine-address boundary even
    // when the individual candidate address is initially within the span.
    const auto wrapped_base =
        std::numeric_limits<std::uintptr_t>::max() & ~std::uintptr_t{7};
    assert(!CompressedActorRef::encode(wrapped_base, 16, wrapped_base));
    assert(!first->decode(wrapped_base, 16));
    assert(!NativeActorManagedRef::encode(wrapped_base, 16, wrapped_base));
    assert(!native->decode(wrapped_base, 16));

    // Exact last address of the machine domain is allowed for an 8-byte span.
    const auto end_base =
        std::numeric_limits<std::uintptr_t>::max() - std::uintptr_t{7};
    auto terminal = CompressedActorRef::encode(end_base, 8, end_base);
    assert(terminal && terminal->decode(end_base, 8) == end_base);

    return 0;
}
