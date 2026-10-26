#include "oreslang/runtime/ManagedRef.hpp"
#include "oreslang/runtime/DopLayout.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <limits>

// Assert macros disappear under NDEBUG; these native ABI tests must not.
#define CHECK(expression) do { \
    if (!(expression)) { \
        std::fprintf(stderr, "managed_ref contract failed: %s at line %d\n", \
                     #expression, __LINE__); \
        return EXIT_FAILURE; \
    } \
} while (false)

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
    CHECK(first && third);
    CHECK(first->storage() == 1);
    CHECK(third->storage() == 3);
    CHECK(first->decode(base, arena.size()).value() == base);
    CHECK(third->decode(base, arena.size()).value() == base + 16);

    auto null_ref = CompressedActorRef::encode(base, arena.size(), 0);
    CHECK(null_ref && null_ref->is_null());
    CHECK(null_ref->decode(base, arena.size()).has_value());
    CHECK(null_ref->decode(base, arena.size()).value() == 0);

    CHECK(!CompressedActorRef::encode(base, arena.size(), base + 3));
    CHECK(!CompressedActorRef::encode(base, arena.size(), base + arena.size()));
    CHECK(!third->decode(base, 8));

    auto native = NativeActorManagedRef::encode(base, arena.size(), base + 24);
    CHECK(native);
    CHECK(native->storage() == static_cast<std::uint64_t>(base + 24));
    CHECK(native->decode(base, arena.size()).value() == base + 24);
    CHECK(!native->decode(base + 32, arena.size() - 32));

    auto entity = DenseIndex32<EntityTag>::from_index(41);
    auto component = DenseIndex32<ComponentTag>::from_index(41);
    CHECK(entity && component);
    CHECK(entity->index().value() == 41);
    CHECK(component->index().value() == 41);
    static_assert(sizeof(DenseIndex32<EntityTag>) == 4);
    static_assert(sizeof(DenseIndex32<ComponentTag>) == 4);
    CHECK(!DenseIndex32<EntityTag>::from_index(
        static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())));

    // DOD contract: compact refs/indices can be laid out as 4-byte scalar
    // columns or AoSoA lanes without changing their logical source-level type.
    oreslang::dop::Plan soa(oreslang::dop::Layout::SoA, 1024, 3, 4, 16, 1);
    oreslang::dop::Plan aosoa(oreslang::dop::Layout::AoSoA, 1024, 3, 4, 16, 8);
    CHECK(soa.byte_offset(1, 0) == 4);
    CHECK(aosoa.byte_offset(1, 0) == 4);
    CHECK(aosoa.byte_offset(0, 1) == 32);

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
        CHECK(last && last->storage() == UINT32_MAX);
        CHECK(last->decode(synthetic_base, span) == synthetic_base + last_offset);

        const auto high_bit_offset = static_cast<std::uintptr_t>(UINT64_C(1) << 34);
        auto high = CompressedActorRef::encode(
            synthetic_base, span, synthetic_base + high_bit_offset);
        CHECK(high && high->storage() > static_cast<std::uint32_t>(INT32_MAX));
        CHECK(high->decode(synthetic_base, span) ==
               synthetic_base + high_bit_offset);

        // Compression must not partially accept a region whose declared
        // extent cannot be encoded. The allocator must select native64 first.
        CHECK(!CompressedActorRef::encode(synthetic_base, span + 8, synthetic_base));
        CHECK(!first->decode(synthetic_base, span + 8));
    }

    // Reject malformed regions crossing the machine-address boundary even
    // when the individual candidate address is initially within the span.
    const auto wrapped_base =
        std::numeric_limits<std::uintptr_t>::max() & ~std::uintptr_t{7};
    CHECK(!CompressedActorRef::encode(wrapped_base, 16, wrapped_base));
    CHECK(!first->decode(wrapped_base, 16));
    CHECK(!NativeActorManagedRef::encode(wrapped_base, 16, wrapped_base));
    CHECK(!native->decode(wrapped_base, 16));

    // Exact last address of the machine domain is allowed for an 8-byte span.
    const auto end_base =
        std::numeric_limits<std::uintptr_t>::max() - std::uintptr_t{7};
    auto terminal = CompressedActorRef::encode(end_base, 8, end_base);
    CHECK(terminal && terminal->decode(end_base, 8) == end_base);

    // Stable physical-reference ABI corpus shared with Java/Graal:
    // ActorCompressed32Reference.encode(offset) must produce the same
    // unsigned wire words. This checks 100k deterministic XorShift64
    // offsets and an FNV64 digest without allocating a 32-GiB arena.
    // Unlike CHECK(), these checks MUST remain active in Release/NDEBUG.
#if UINTPTR_MAX > UINT32_MAX
    constexpr std::uintptr_t parity_base = UINT64_C(0x100000000);
    const auto parity_span = static_cast<std::size_t>(
        CompressedActorRef::max_addressable_span);
    std::uint64_t parity_rng = UINT64_C(0x6a09e667f3bcc909);
    std::uint64_t parity_digest = UINT64_C(0xcbf29ce484222325);
    for (std::uint32_t n = 0; n < 100000; ++n) {
        parity_rng ^= parity_rng << 13;
        parity_rng ^= parity_rng >> 7;
        parity_rng ^= parity_rng << 17;
        const auto offset = (parity_rng % UINT64_C(0xffffffff)) * 8;
        auto compact = CompressedActorRef::encode(
            parity_base, parity_span, parity_base + offset);
        if (!compact) return 10;
        auto round_trip = compact->decode(parity_base, parity_span);
        if (!round_trip || *round_trip != parity_base + offset) return 11;
        parity_digest = (parity_digest ^ compact->storage())
            * UINT64_C(1099511628211);
    }
    // Expected digest computed independently with Java actor offset encoding.
    if (parity_digest != UINT64_C(0x46402ba55da7cde5)) return 12;
#endif

    return 0;
}
