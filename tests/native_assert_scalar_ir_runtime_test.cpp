#include "oreslang/runtime/NativeAssertScalarAbi.hpp"

#include <cstdint>
#include <limits>
#include <stdexcept>

using oreslang::EffectKind;
using namespace oreslang::assert_scalar_abi;

extern "C" void oreslang_assert_equal_scalar_v0(
    const Scalar*, const Scalar*, std::uint8_t, BoolResult*);
extern "C" void oreslang_assert_trap_bool_v0(
    const BoolResult*, BoolResult*);

#define CHECK(p) do { if (!(p)) throw std::runtime_error("failed: " #p); } while(false)

static void match(BoolResult actual, BoolResult expected) {
    CHECK(valid(actual));
    CHECK(actual.code == expected.code);
    CHECK(actual.effect == expected.effect);
    CHECK(actual.present == expected.present);
    CHECK(actual.value == expected.value);
    CHECK(actual.reserved == expected.reserved);
}

static void compare(Scalar a, Scalar b, std::uint8_t throwing) {
    struct Guarded {
        std::uint64_t before = 0xACEDFACE12345678ull;
        BoolResult result = {0xDEADBEEFu, 0xff, 0xff, 0xff, 0xff};
        std::uint64_t after = 0x0123456789ABCDEFull;
    } guarded;
    oreslang_assert_equal_scalar_v0(&a, &b, throwing, &guarded.result);
    CHECK(guarded.before == 0xACEDFACE12345678ull);
    CHECK(guarded.after == 0x0123456789ABCDEFull);
    const BoolResult actual = guarded.result;
    const BoolResult expected = throwing > 1 ? malformed() :
        equal(a, b, throwing == 1);
    match(actual, expected);
}

static void trap_compare(BoolResult status) {
    BoolResult actual = {0xDEADBEEFu, 0xff, 0xff, 0xff, 0xff};
    oreslang_assert_trap_bool_v0(&status, &actual);
    match(actual, trap(status));
    // Emitted code must load input before writing its output. This is a
    // defined caller-owned in-place transformation, not a stale-read branch.
    BoolResult in_place = status;
    oreslang_assert_trap_bool_v0(&in_place, &in_place);
    match(in_place, trap(status));
}

int main() {
    compare(i64(10), i64(10), 0);
    compare(i64(10), i64(10), 1);
    compare(i64(10), i64(20), 0);
    compare(i64(10), i64(20), 1);
    compare(i64(10), i64(10), 2);
    compare(i64(-1), i64(-1), 1);
    compare(i64(std::numeric_limits<std::int64_t>::min()),
            i64(std::numeric_limits<std::int64_t>::max()), 1);
    compare(boolean(true), boolean(true), 1);
    compare(boolean(true), boolean(false), 1);
    compare(i64(1), boolean(true), 1);

    for (unsigned tag = 0; tag < 256; ++tag) {
        Scalar value = i64(1);
        value.tag = static_cast<std::uint8_t>(tag);
        compare(value, i64(1), 0);
        compare(i64(1), value, 1);
        BoolResult forged = some(true);
        forged.effect = static_cast<std::uint8_t>(tag);
        trap_compare(forged);
    }
    Scalar corrupted = i64(7);
    for (auto& byte : corrupted.reserved) {
        byte = 1;
        compare(corrupted, i64(7), 1);
        byte = 0;
    }
    auto malformed_bool = boolean(true);
    malformed_bool.bits = 2;
    compare(malformed_bool, boolean(true), 1);

    const BoolResult samples[] = {
        some(true), some(false), none(),
        effect(EffectKind::Throw, 1),
        effect(EffectKind::Raise, 6),
        effect(EffectKind::Panic, 7),
        effect(EffectKind::Cancel, 8),
        effect(EffectKind::Fatal, 9),
        effect(EffectKind::ProcessExit, 0),
        effect(EffectKind::ProcessExit, 10),
        malformed()
    };
    for (auto sample : samples) trap_compare(sample);
    auto corrupt = none();
    corrupt.value = 1;
    trap_compare(corrupt);
    corrupt = some(true);
    corrupt.reserved = 1;
    trap_compare(corrupt);
    corrupt = some(true);
    corrupt.code = 42;
    trap_compare(corrupt);
    corrupt = effect(EffectKind::Throw, 4);
    corrupt.present = 1;
    trap_compare(corrupt);

    // Deterministic, reproducible differential fuzz across valid and
    // hostile scalar tags, reserved bytes, payloads, throw flags and
    // effect-result fields. All observed IR results must exactly match
    // the merged NativeAssertScalarAbi.hpp implementation at O0 and O2.
    std::uint64_t seed = 0xF17E0B1D5EED1234ull;
    auto next = [&seed]() -> std::uint64_t {
        seed ^= seed >> 12;
        seed ^= seed << 25;
        seed ^= seed >> 27;
        return seed * 0x2545F4914F6CDD1Dull;
    };
    for (unsigned iteration = 0; iteration < 16384; ++iteration) {
        Scalar a = i64(static_cast<std::int64_t>(next()));
        Scalar b = i64(static_cast<std::int64_t>(next()));
        if (iteration % 4 == 0) {
            a = boolean((next() & 1u) != 0);
            b = boolean((next() & 1u) != 0);
        } else if (iteration % 4 == 1) {
            b = a;
        } else if (iteration % 4 == 2) {
            a.tag = static_cast<std::uint8_t>(next());
        } else {
            b.reserved[(next() % b.reserved.size())] =
                static_cast<std::uint8_t>((next() % 255u) + 1u);
        }
        compare(a, b, static_cast<std::uint8_t>(next() % 4u));
        BoolResult tested = effect(EffectKind::Throw,
                                   static_cast<std::uint32_t>(next()));
        if (iteration % 8 == 0)
            tested = some((next() & 1u) != 0);
        if (iteration % 8 == 1)
            tested.effect = static_cast<std::uint8_t>(next());
        if (iteration % 8 == 2)
            tested.present = static_cast<std::uint8_t>(next());
        if (iteration % 8 == 3)
            tested.value = static_cast<std::uint8_t>(next());
        if (iteration % 8 == 4)
            tested.reserved = static_cast<std::uint8_t>(next());
        if (iteration % 8 == 5)
            tested.code = static_cast<std::uint32_t>(next());
        if (iteration % 8 == 6)
            tested = effect(EffectKind::ProcessExit, 0);
        if (iteration % 8 == 7)
            tested = effect(EffectKind::Fatal,
                            static_cast<std::uint32_t>(next()));
        trap_compare(tested);
    }
}
