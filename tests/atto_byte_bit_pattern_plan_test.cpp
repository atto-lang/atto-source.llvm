#include "oreslang/frontend/NativeByteBitPatternPlan.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <type_traits>

using namespace oreslang;

#define CHECK(x) do { if (!(x)) { \
    std::fprintf(stderr, "native byte/bit pattern failed: %s:%d: %s\\n", __FILE__, __LINE__, #x); \
    std::abort(); } } while (false)

int main() {
    static_assert(std::is_trivially_copyable_v<NativePatternByteSpan>);
    static_assert(std::is_trivially_copyable_v<NativePatternBitSpan>);
    constexpr auto eq = NativePatternElement::eq;
    constexpr auto cap = NativePatternElement::capture;
    constexpr auto skip = NativePatternElement::ignore;

    const std::array<std::uint8_t, 4> get{{71, 69, 84, 33}};
    const NativePatternByteSpan source{get.data(), get.size()};
    constexpr std::array<NativePatternElement, 3> prefix{{eq(71), eq(69), eq(84)}};
    auto matched = native_match_bytes(source, prefix, true);
    CHECK(matched.matched && matched.tail.size == 1 && matched.tail.data[0] == 33);
    CHECK(!native_match_bytes(source, prefix).matched);
    CHECK(!native_match_bytes(source,
            std::array<NativePatternElement, 3>{{eq(80), eq(69), eq(84)}}, true).matched);

    const auto captures = native_match_bytes(source,
            std::array<NativePatternElement, 3>{{cap(), skip(), eq(84)}}, true);
    CHECK(captures.matched && captures.captured[0] == 71 && captures.tail.size == 1);
    const auto one = std::array<std::uint8_t, 1>{{3}};
    CHECK(native_match_bytes(NativePatternByteSpan{one.data(), 1},
            std::array<NativePatternElement, 1>{{eq(3)}}).matched);
    CHECK(!native_match_bytes(source,
            std::array<NativePatternElement, 1>{{eq(71)}}).matched);
    CHECK(!native_match_bytes(source,
            std::array<NativePatternElement, 1>{{eq(71),}}, false).matched);

    const NativePatternByteSpan empty{};
    CHECK(native_match_bytes(empty, std::array<NativePatternElement, 0>{}).matched);
    CHECK(!native_match_bytes(empty, std::array<NativePatternElement, 0>{}, true).matched);
    CHECK(!native_match_bytes(NativePatternByteSpan{nullptr, 10}, prefix, true).matched);

    const std::array<std::uint8_t, 1> flags{{0xa5}};
    NativePatternBitSpan bits{{flags.data(), flags.size()}, 0, 8};
    CHECK(bits.valid());
    CHECK(native_match_bits(bits, std::array<NativePatternElement, 8>{{
            eq(1), eq(0), eq(1), eq(0), eq(0), eq(1), eq(0), eq(1)}}).matched);
    CHECK(!native_match_bits(bits,
            std::array<NativePatternElement, 1>{{eq(1)}}).matched);
    const auto b = native_match_bits(bits,
            std::array<NativePatternElement, 3>{{eq(1), eq(0), eq(1)}}, true);
    CHECK(b.matched && b.tail.valid() && b.tail.offset == 3 && b.tail.length == 5);
    const auto c = native_match_bits(b.tail,
            std::array<NativePatternElement, 2>{{eq(0), eq(0)}}, true);
    CHECK(c.matched && c.tail.offset == 5 && c.tail.length == 3);
    CHECK(!native_match_bits(bits,
            std::array<NativePatternElement, 1>{{eq(2)}}, true).matched);
    CHECK(!native_match_bits(bits,
            std::array<NativePatternElement, 0>{}, true).matched);

    CHECK((!NativePatternBitSpan{{nullptr, 1}, 0, 1}.valid()));
    CHECK((!NativePatternBitSpan{{flags.data(), 1}, 7, 2}.valid()));
    CHECK((!NativePatternBitSpan{{flags.data(), std::numeric_limits<std::size_t>::max()}, 0, 1}.valid()));

    // Network packet bytes: version=1, BE length=3, payload="abc", rest=0xff.
    const std::array<std::uint8_t, 7> packet{{1, 0, 3, 97, 98, 99, 255}};
    NativeBinaryPatternCursor cursor({packet.data(), packet.size()});
    std::uint64_t value = 777;
    CHECK(cursor.take_uint_bytes(8, false, value) && value == 1);
    CHECK(cursor.take_uint_bytes(16, false, value) && value == 3);
    NativePatternByteSpan payload{};
    CHECK(cursor.take_bytes(static_cast<std::size_t>(value), payload));
    CHECK(payload.size == 3 && payload.data[0] == 97 && payload.data[2] == 99);
    CHECK(!cursor.exact());
    NativePatternByteSpan rest{};
    CHECK(cursor.remaining_bytes(rest) && rest.size == 1 && rest.data[0] == 255);
    CHECK(cursor.exact());

    const std::array<std::uint8_t, 2> little{{0x34, 0x12}};
    NativeBinaryPatternCursor le({little.data(),little.size()});
    CHECK(le.take_uint_bytes(16, true, value) && value == 0x1234);
    CHECK(le.exact());

    NativeBinaryPatternCursor fields({flags.data(),flags.size()});
    CHECK(fields.take_bits(3, value) && value == 5);
    CHECK(fields.take_bits(5, value) && value == 5);
    CHECK(fields.exact());

    NativeBinaryPatternCursor unaligned({flags.data(),flags.size()});
    CHECK(unaligned.take_bits(3, value) && value == 5);
    const auto unchanged = value;
    NativePatternByteSpan untouched{flags.data(),123};
    CHECK(!unaligned.take_uint_bytes(16, false, value) && value == unchanged);
    CHECK(!unaligned.take_bytes(1, untouched));
    CHECK(untouched.size == 123);
    CHECK(!unaligned.remaining_bytes(untouched));
    CHECK(unaligned.bit_offset() == 3);

    NativeBinaryPatternCursor short_input({packet.data(), 2});
    CHECK(short_input.take_uint_bytes(8, false, value));
    const auto before = value;
    CHECK(!short_input.take_uint_bytes(16, false, value) && value == before);
    CHECK(!short_input.take_bytes(std::numeric_limits<std::size_t>::max(), untouched));
    CHECK(!short_input.exact());
    NativeBinaryPatternCursor invalid({nullptr, 100});
    CHECK(!invalid.take_bits(8, value));
    CHECK(!invalid.remaining_bytes(untouched));

    // Non-matching inputs must not read past the end or touch the tail.
    for (std::size_t i = 0; i <= get.size(); ++i) {
        const auto partial = native_match_bytes(
                NativePatternByteSpan{get.data(), i}, prefix, true);
        CHECK(partial.matched == (i >= 3));
    }

    std::puts("ATTO_NATIVE_AOT_STATIC_BYTE_BIT_PLAN_PASS HOST_ONLY_NOT_GUEST_AOT");
    return 0;
}
