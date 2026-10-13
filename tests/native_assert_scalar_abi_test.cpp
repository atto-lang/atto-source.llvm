#include "oreslang/runtime/NativeAssertScalarAbi.hpp"

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <type_traits>

#define CHECK(p) do { if (!(p)) throw std::runtime_error("failed: " #p); } while(false)

using namespace oreslang::assert_scalar_abi;
using oreslang::EffectKind;

static_assert(valid(i64(0)));
static_assert(valid(boolean(true)));
static_assert(equal(i64(3), i64(3), true).present == 1);
static_assert(trap(equal(i64(3), i64(4), true)).present == 0);
static_assert(trap(effect(EffectKind::Panic, 99)).effect ==
              static_cast<std::uint8_t>(EffectKind::Panic));

int main() {
    const auto success = equal(i64(10), i64(10), true);
    CHECK(valid(success));
    CHECK(success.effect == static_cast<std::uint8_t>(EffectKind::Return));
    CHECK(success.present == 1 && success.value == 1);
    CHECK(trap(success).present == 1 && trap(success).value == 1);

    const auto ordinary = equal(i64(10), i64(20), true);
    CHECK(valid(ordinary));
    CHECK(ordinary.effect == static_cast<std::uint8_t>(EffectKind::Throw));
    CHECK(ordinary.present == 0);
    const auto recovered = trap(ordinary);
    CHECK(valid(recovered));
    CHECK(recovered.effect == static_cast<std::uint8_t>(EffectKind::Return));
    CHECK(recovered.present == 0 && recovered.value == 0);

    const auto predicate_false = equal(i64(10), i64(20));
    CHECK(predicate_false.effect == static_cast<std::uint8_t>(EffectKind::Return));
    CHECK(predicate_false.present == 1 && predicate_false.value == 0);
    CHECK(trap(predicate_false).present == 1); // Some(false) != None

    CHECK(equal(boolean(true), boolean(true)).value == 1);
    CHECK(equal(boolean(false), boolean(true)).value == 0);
    CHECK(equal(i64(1), boolean(true)).value == 0);
    CHECK(equal(i64(0), boolean(false)).value == 0);
    CHECK(equal(i64(-1), i64(-1)).value == 1);
    CHECK(equal(i64(std::numeric_limits<std::int64_t>::min()),
                i64(std::numeric_limits<std::int64_t>::max())).value == 0);

    // Zero all reserved fields and reject unsupported Any tags at the ABI,
    // rather than coercing malformed data into false, success or Option.None.
    for (unsigned raw = 0; raw < 256; ++raw) {
        auto lhs = i64(0);
        lhs.tag = static_cast<std::uint8_t>(raw);
        CHECK(valid(lhs) == (raw == 1 || raw == 2));
        const auto actual = equal(lhs, i64(0), true);
        if (raw != 1 && raw != 2) {
            CHECK(actual.effect == static_cast<std::uint8_t>(EffectKind::Fatal));
            CHECK(actual.code == 0xFFFFFFFDu);
            CHECK(trap(actual).effect == static_cast<std::uint8_t>(EffectKind::Fatal));
        }
        BoolResult forged = some(true);
        forged.effect = static_cast<std::uint8_t>(raw);
        if (raw > static_cast<unsigned>(EffectKind::ProcessExit)) {
            CHECK(!valid(forged));
            CHECK(trap(forged).effect == static_cast<std::uint8_t>(EffectKind::Fatal));
        }
    }

    auto wrong_boolean = boolean(true);
    wrong_boolean.bits = 2;
    CHECK(!valid(wrong_boolean));
    CHECK(equal(wrong_boolean, boolean(true)).effect ==
          static_cast<std::uint8_t>(EffectKind::Fatal));
    auto hidden = i64(3);
    hidden.reserved[6] = 1;
    CHECK(!valid(hidden));
    auto forged_return = some(true);
    forged_return.reserved = 1;
    CHECK(!valid(forged_return));
    CHECK(trap(forged_return).effect ==
          static_cast<std::uint8_t>(EffectKind::Fatal));
    auto forged_none = none();
    forged_none.value = 1;
    CHECK(!valid(forged_none));

    constexpr EffectKind terminal[] = {
        EffectKind::Raise, EffectKind::Panic, EffectKind::Cancel,
        EffectKind::Fatal, EffectKind::ProcessExit
    };
    for (const auto kind : terminal) {
        const auto pending = effect(kind, 77);
        CHECK(valid(pending));
        const auto outcome = trap(pending);
        CHECK(outcome.effect == static_cast<std::uint8_t>(kind));
        CHECK(outcome.code == 77 && outcome.present == 0);
    }
    const auto exit0 = trap(effect(EffectKind::ProcessExit, 0));
    CHECK(exit0.effect == static_cast<std::uint8_t>(EffectKind::ProcessExit));
    CHECK(exit0.present == 0);
}
