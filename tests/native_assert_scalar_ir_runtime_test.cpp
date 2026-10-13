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
    BoolResult actual{};
    oreslang_assert_equal_scalar_v0(&a, &b, throwing, &actual);
    const BoolResult expected = throwing > 1 ? malformed() :
        equal(a, b, throwing == 1);
    match(actual, expected);
}

static void trap_compare(BoolResult status) {
    BoolResult actual{};
    oreslang_assert_trap_bool_v0(&status, &actual);
    match(actual, trap(status));
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
}
