#include "oreslang/runtime/StrictSigned64Arithmetic.hpp"

#include <cstdint>
#include <cstring>
#include <limits>
#include <initializer_list>

using oreslang::runtime::StrictSigned64Arithmetic;
using oreslang::runtime::StrictIntegerFailure;
using I = std::int64_t;

static_assert(StrictSigned64Arithmetic::rem(-9, 4).value == -1);
static_assert(StrictSigned64Arithmetic::mod(-9, 4).value == 3);
static_assert(StrictSigned64Arithmetic::mod(-9, -4).value == 3);
static_assert(StrictSigned64Arithmetic::mod(
    -1, std::numeric_limits<I>::min()).value == std::numeric_limits<I>::max());
static_assert(StrictSigned64Arithmetic::rem(
    std::numeric_limits<I>::min(), -1).value == 0);
static_assert(StrictSigned64Arithmetic::pow(0, 0).value == 1);
static_assert(StrictSigned64Arithmetic::pow(-2, 63).value
    == std::numeric_limits<I>::min());
static_assert(StrictSigned64Arithmetic::pow(2, 63).failure
    == StrictIntegerFailure::overflow);
static_assert(StrictSigned64Arithmetic::mod(2, 0).failure
    == StrictIntegerFailure::divisor_zero);
static_assert(StrictSigned64Arithmetic::pow(2, -1).failure
    == StrictIntegerFailure::negative_exponent);
static_assert(StrictSigned64Arithmetic::bitXor(5, 3).value == 6);

// Source fixture: oreslang-source.java/contracts/v1/strict-named-integer-arithmetic-v1.json
// Exact Java fixture blob 3f65f557f06e8cd0d34a738a0d357d6c2e477665; every case below is copied by
// operation/argument/value/error, not inferred from the backend-under-test.
// NOTE: host-only oracle, not compiler guest code generation evidence.
struct Vector {
    const char* name;
    oreslang::runtime::StrictIntegerResult (*operation)(I, I) noexcept;
    I left;
    I right;
    I expected;
    StrictIntegerFailure error;
};

static constexpr Vector cases[] = {
        {"rem_pos_pos", &StrictSigned64Arithmetic::rem, I(9), I(4), I(1), StrictIntegerFailure::none},
        {"rem_neg_pos", &StrictSigned64Arithmetic::rem, I(-9), I(4), I(-1), StrictIntegerFailure::none},
        {"rem_pos_neg", &StrictSigned64Arithmetic::rem, I(9), I(-4), I(1), StrictIntegerFailure::none},
        {"rem_neg_neg", &StrictSigned64Arithmetic::rem, I(-9), I(-4), I(-1), StrictIntegerFailure::none},
        {"rem_min_minus_one", &StrictSigned64Arithmetic::rem, std::numeric_limits<I>::min(), I(-1), I(0), StrictIntegerFailure::none},
        {"rem_min_max", &StrictSigned64Arithmetic::rem, std::numeric_limits<I>::min(), std::numeric_limits<I>::max(), I(-1), StrictIntegerFailure::none},
        {"rem_zero", &StrictSigned64Arithmetic::rem, I(9), I(0), I(0), StrictIntegerFailure::divisor_zero},
        {"mod_pos_pos", &StrictSigned64Arithmetic::mod, I(9), I(4), I(1), StrictIntegerFailure::none},
        {"mod_neg_pos", &StrictSigned64Arithmetic::mod, I(-9), I(4), I(3), StrictIntegerFailure::none},
        {"mod_pos_neg", &StrictSigned64Arithmetic::mod, I(9), I(-4), I(1), StrictIntegerFailure::none},
        {"mod_neg_neg", &StrictSigned64Arithmetic::mod, I(-9), I(-4), I(3), StrictIntegerFailure::none},
        {"mod_min_minus_one", &StrictSigned64Arithmetic::mod, std::numeric_limits<I>::min(), I(-1), I(0), StrictIntegerFailure::none},
        {"mod_neg_one_min", &StrictSigned64Arithmetic::mod, I(-1), std::numeric_limits<I>::min(), std::numeric_limits<I>::max(), StrictIntegerFailure::none},
        {"mod_min_max", &StrictSigned64Arithmetic::mod, std::numeric_limits<I>::min(), std::numeric_limits<I>::max(), I(9223372036854775806), StrictIntegerFailure::none},
        {"mod_zero", &StrictSigned64Arithmetic::mod, I(-1), I(0), I(0), StrictIntegerFailure::divisor_zero},
        {"pow_zero_zero", &StrictSigned64Arithmetic::pow, I(0), I(0), I(1), StrictIntegerFailure::none},
        {"pow_negative_odd", &StrictSigned64Arithmetic::pow, I(-2), I(3), I(-8), StrictIntegerFailure::none},
        {"pow_min_exact", &StrictSigned64Arithmetic::pow, I(-2), I(63), std::numeric_limits<I>::min(), StrictIntegerFailure::none},
        {"pow_long_max_exponent_neg_one", &StrictSigned64Arithmetic::pow, I(-1), std::numeric_limits<I>::max(), I(-1), StrictIntegerFailure::none},
        {"pow_zero_very_large", &StrictSigned64Arithmetic::pow, I(0), std::numeric_limits<I>::max(), I(0), StrictIntegerFailure::none},
        {"pow_overflow", &StrictSigned64Arithmetic::pow, I(2), I(63), I(0), StrictIntegerFailure::overflow},
        {"pow_negative_exponent", &StrictSigned64Arithmetic::pow, I(2), I(-1), I(0), StrictIntegerFailure::negative_exponent},
        {"xor_zero", &StrictSigned64Arithmetic::bitXor, I(0), I(0), I(0), StrictIntegerFailure::none},
        {"xor_small", &StrictSigned64Arithmetic::bitXor, I(5), I(3), I(6), StrictIntegerFailure::none},
        {"xor_negative", &StrictSigned64Arithmetic::bitXor, I(-1), I(0), I(-1), StrictIntegerFailure::none},
        {"xor_min_max", &StrictSigned64Arithmetic::bitXor, std::numeric_limits<I>::min(), std::numeric_limits<I>::max(), I(-1), StrictIntegerFailure::none},
};

static_assert(sizeof(cases) / sizeof(cases[0]) == 26);

// Cross-backend IEEE754 i64->f64 exact-conversion vectors from Java #2193.
// Original fixture blob SHA a43767241e392189a0bab0c66e4b6ae086bbcdb0.
struct FloatVector {
    const char* name;
    I input;
    std::uint64_t bits;
    StrictIntegerFailure failure;
};
static constexpr FloatVector floatCases[] = {
        {"zero", I(0), 0x0000000000000000ULL, StrictIntegerFailure::none},
        {"positive", I(42), 0x4045000000000000ULL, StrictIntegerFailure::none},
        {"negative", I(-42), 0xc045000000000000ULL, StrictIntegerFailure::none},
        {"two_to_53", I(9007199254740992), 0x4340000000000000ULL, StrictIntegerFailure::none},
        {"above_exact_limit", I(9007199254740993), 0ULL, StrictIntegerFailure::inexact_conversion},
        {"minimum", std::numeric_limits<I>::min(), 0xc3e0000000000000ULL, StrictIntegerFailure::none},
        {"near_minimum", I(-9223372036854775807), 0ULL, StrictIntegerFailure::inexact_conversion},
        {"maximum", std::numeric_limits<I>::max(), 0ULL, StrictIntegerFailure::inexact_conversion},
        {"two_to_62", I(4611686018427387904), 0x43d0000000000000ULL, StrictIntegerFailure::none},
        {"two_to_62_plus_spacing", I(4611686018427388928), 0x43d0000000000001ULL, StrictIntegerFailure::none},
        {"two_to_62_plus_one", I(4611686018427387905), 0ULL, StrictIntegerFailure::inexact_conversion},
};
static_assert(sizeof(floatCases) / sizeof(floatCases[0]) == 11);
static_assert(StrictSigned64Arithmetic::toFloatExact(42).value == 42.0);
static_assert(StrictSigned64Arithmetic::toFloatExact(std::numeric_limits<I>::max()).failure
    == StrictIntegerFailure::inexact_conversion);


// Independent bounded-magnitude exponent oracle. Unlike the production
// exponentiation-by-squaring implementation, this multiplies one factor at
// a time in unsigned space and checks the signed result bound before each
// multiplication. In particular, |INT64_MIN| is never formed as signed.
static std::uint64_t reference_magnitude(I value) noexcept {
    const auto bits = static_cast<std::uint64_t>(value);
    return value < 0 ? std::uint64_t(0) - bits : bits;
}
static oreslang::runtime::StrictIntegerResult reference_pow(I base, I exponent) noexcept {
    if (exponent < 0) return {0, StrictIntegerFailure::negative_exponent};
    if (exponent == 0) return {1, StrictIntegerFailure::none};
    if (base == 0) return {0, StrictIntegerFailure::none};
    if (base == 1) return {1, StrictIntegerFailure::none};
    if (base == -1) return {(exponent & 1) ? I(-1) : I(1), StrictIntegerFailure::none};
    // For |base| >= 2, exponent >= 64 cannot fit in signed 64 bits.
    if (exponent >= 64) return {0, StrictIntegerFailure::overflow};
    const bool negative = base < 0 && (exponent & 1);
    constexpr auto negative_limit = std::uint64_t(1) << 63U;
    const auto limit = negative ? negative_limit
        : static_cast<std::uint64_t>(std::numeric_limits<I>::max());
    const auto factor = reference_magnitude(base);
    std::uint64_t product = 1;
    for (I i = 0; i < exponent; ++i) {
        if (product > limit / factor) return {0, StrictIntegerFailure::overflow};
        product *= factor;
    }
    if (negative && product == negative_limit) {
        return {std::numeric_limits<I>::min(), StrictIntegerFailure::none};
    }
    const auto positive = static_cast<I>(product); // product < 2^63
    return {negative ? -positive : positive, StrictIntegerFailure::none};
}

// Exact binary64 integer conversion can be determined without floating
// arithmetic: low bits below the 53-bit significand must all be zero.
static bool reference_float_exact(I value) noexcept {
    const auto magnitude = reference_magnitude(value);
    unsigned width = 0;
    for (auto bits = magnitude; bits != 0; bits >>= 1U) ++width;
    if (width <= 53U) return true;
    const auto lost_bits = width - 53U; // at most 11 for uint64_t
    const auto mask = (std::uint64_t(1) << lost_bits) - 1U;
    return (magnitude & mask) == 0;
}

static bool run_extra_reference_checks() noexcept {
    constexpr I lo = std::numeric_limits<I>::min();
    constexpr I hi = std::numeric_limits<I>::max();
    const auto verify = [](I base, I exponent) noexcept {
        const auto expected = reference_pow(base, exponent);
        const auto actual = StrictSigned64Arithmetic::pow(base, exponent);
        return expected.failure == actual.failure
            && (!expected.ok() || expected.value == actual.value);
    };
    const auto verify_float = [](I value) noexcept {
        const auto actual = StrictSigned64Arithmetic::toFloatExact(value);
        const bool exact = reference_float_exact(value);
        return exact ? (actual.ok() && actual.value == static_cast<double>(value))
            : actual.failure == StrictIntegerFailure::inexact_conversion;
    };
    for (I base : {lo, lo + 1, I(-1000), I(-10), I(-3), I(-2), I(-1), I(0),
                   I(1), I(2), I(3), I(10), I(1000), hi - 1, hi}) {
        for (I exponent : {I(-2), I(-1), I(0), I(1), I(2), I(3), I(4), I(7),
                           I(8), I(15), I(31), I(32), I(62), I(63), I(64), hi}) {
            if (!verify(base, exponent)) return false;
        }
    }
    for (I value : {lo, lo + 1, lo + 1024, lo + 2048,
                    I(-9007199254740993), I(-9007199254740992), I(-1), I(0),
                    I(1), I(9007199254740992), I(9007199254740993),
                    hi - 2047, hi - 1023, hi}) {
        if (!verify_float(value)) return false;
    }
    // Deterministic full-width inputs, including negative and high-bit cases.
    std::uint64_t state = 0xd1b54a32d192ed03ULL;
    const auto next = [&state]() noexcept {
        state ^= state << 13U;
        state ^= state >> 7U;
        state ^= state << 17U;
        return state;
    };
    for (int i = 0; i < 8192; ++i) {
        const auto bits = next();
        I base = 0;
        std::memcpy(&base, &bits, sizeof(base));
        const I exponent = static_cast<I>(next() % 70U) - I(2);
        if (!verify(base, exponent) || !verify_float(base)) return false;
    }

    // Independent full-width XOR oracle: derive each result using only
    // unsigned OR/AND/NOT Boolean algebra, never the production '^' operator.
    // This catches signed high-bit and two's-complement mistakes that the four
    // fixed cross-backend vectors alone cannot cover.
    for (int i = 0; i < 8192; ++i) {
        const std::uint64_t left_bits = next();
        const std::uint64_t right_bits = next();
        I left = 0;
        I right = 0;
        std::memcpy(&left, &left_bits, sizeof(left));
        std::memcpy(&right, &right_bits, sizeof(right));
        const std::uint64_t expected_bits =
            (left_bits | right_bits) & ~(left_bits & right_bits);
        I expected = 0;
        std::memcpy(&expected, &expected_bits, sizeof(expected));
        const auto actual = StrictSigned64Arithmetic::bitXor(left, right);
        if (!actual.ok() || actual.value != expected) return false;
        const auto commuted = StrictSigned64Arithmetic::bitXor(right, left);
        if (!commuted.ok() || commuted.value != expected) return false;
        const auto restored = StrictSigned64Arithmetic::bitXor(actual.value, right);
        if (!restored.ok() || restored.value != left) return false;
    }
    return true;
}

int main() {
    static_assert(sizeof(double) == sizeof(std::uint64_t));
    static_assert(std::numeric_limits<double>::is_iec559);
    for (const auto& item : floatCases) {
        const auto actual = StrictSigned64Arithmetic::toFloatExact(item.input);
        if (actual.failure != item.failure) return 13;
        if (actual.ok()) {
            std::uint64_t bits = 0;
            std::memcpy(&bits, &actual.value, sizeof(bits));
            if (bits != item.bits) return 14;
        }
    }
    for (const auto& item : cases) {
        auto actual = item.operation(item.left, item.right);
        if (actual.failure != item.error) return 11;
        if (actual.ok() && actual.value != item.expected) return 12;
    }
    for (I a : {I(-9), I(-1), I(0), I(1), I(9)}) {
        for (I b : {I(-4), I(1), I(4)}) {
            const auto r = StrictSigned64Arithmetic::rem(a, b);
            const auto m = StrictSigned64Arithmetic::mod(a, b);
            if (!r.ok() || !m.ok() || m.value < 0) return 1;
            if (r.value < 0 && m.value != (b > 0 ? r.value + b : r.value - b)) return 2;
        }
    }
    // Independent full-width oracle: compute magnitudes in unsigned space,
    // including |INT64_MIN|, then reconstruct signed remainder and nonnegative
    // Euclidean modulus. Never take signed abs(MIN) or evaluate MIN % -1.
    // This checks the runtime helper rather than deriving expected values
    // from that helper's signed division path.
    const auto unsigned_magnitude = [](I value) noexcept -> std::uint64_t {
        const auto bits = static_cast<std::uint64_t>(value);
        return value < 0 ? std::uint64_t(0) - bits : bits;
    };
    const auto verify_pair = [&](I dividend, I divisor) noexcept -> bool {
        const auto remainder = StrictSigned64Arithmetic::rem(dividend, divisor);
        const auto modulus = StrictSigned64Arithmetic::mod(dividend, divisor);
        if (divisor == 0) {
            return remainder.failure == StrictIntegerFailure::divisor_zero
                && modulus.failure == StrictIntegerFailure::divisor_zero;
        }
        if (!remainder.ok() || !modulus.ok()) return false;
        const auto magnitude = unsigned_magnitude(dividend)
            % unsigned_magnitude(divisor);
        // A nonzero remainder magnitude is strictly below 2^63 and thus
        // safely representable as signed i64, even for divisor INT64_MIN.
        const I signed_magnitude = static_cast<I>(magnitude);
        const I expected_remainder = dividend < 0 ? -signed_magnitude
            : signed_magnitude;
        const I expected_modulus = dividend < 0 && magnitude != 0
            ? static_cast<I>(unsigned_magnitude(divisor) - magnitude)
            : signed_magnitude;
        return remainder.value == expected_remainder
            && modulus.value == expected_modulus
            && modulus.value >= 0;
    };
    constexpr I lo = std::numeric_limits<I>::min();
    constexpr I hi = std::numeric_limits<I>::max();
    for (I a : {lo, lo + 1, I(-9), I(-2), I(-1), I(0), I(1), I(2), I(9), hi - 1, hi}) {
        for (I b : {lo, lo + 1, I(-9), I(-2), I(-1), I(0), I(1), I(2), I(9), hi - 1, hi}) {
            if (!verify_pair(a, b)) return 15;
        }
    }
    // Reproducible full-width stress without implementation-defined
    // unsigned-to-signed narrowing; memcpy preserves the native bit pattern.
    std::uint64_t seed = 0x9e3779b97f4a7c15ULL;
    const auto next_bits = [&seed]() noexcept -> std::uint64_t {
        seed ^= seed << 13U;
        seed ^= seed >> 7U;
        seed ^= seed << 17U;
        return seed;
    };
    for (int i = 0; i < 8192; ++i) {
        const auto a_bits = next_bits();
        const auto b_bits = next_bits();
        I a = 0, b = 0;
        std::memcpy(&a, &a_bits, sizeof(a));
        std::memcpy(&b, &b_bits, sizeof(b));
        if (!verify_pair(a, b)) return 16;
    }
    if (!run_extra_reference_checks()) return 17;
    return 0;
}
