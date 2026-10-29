#pragma once

#include <cstdint>
#include <limits>

namespace oreslang::runtime {

// Pure, allocation-free signed-64 reference semantics. The LLVM compiler
// must separately bind typechecked guest Integer.* names to these operations.
// This helper is NOT evidence of source-language/native AOT parity.
enum class StrictIntegerFailure : std::uint8_t {
    none, divisor_zero, overflow, negative_exponent, inexact_conversion
};

struct StrictIntegerResult {
    std::int64_t value;
    StrictIntegerFailure failure;

    constexpr bool ok() const noexcept {
        return failure == StrictIntegerFailure::none;
    }
};

struct StrictFloatResult {
    double value;
    StrictIntegerFailure failure;

    constexpr bool ok() const noexcept {
        return failure == StrictIntegerFailure::none;
    }
};

struct StrictSigned64Arithmetic {
    using I = std::int64_t;

    static constexpr StrictIntegerResult error(StrictIntegerFailure f) noexcept {
        return {0, f};
    }

    // Truncating remainder. Avoid MIN/-1 hardware div trap/undefined behavior.
    static constexpr StrictIntegerResult rem(I dividend, I divisor) noexcept {
        if (divisor == 0) return error(StrictIntegerFailure::divisor_zero);
        if (dividend == std::numeric_limits<I>::min() && divisor == -1) {
            return {0, StrictIntegerFailure::none};
        }
        return {dividend % divisor, StrictIntegerFailure::none};
    }

    // Euclidean modulus, always nonnegative and less than |divisor|.
    static constexpr StrictIntegerResult mod(I dividend, I divisor) noexcept {
        const auto result = rem(dividend, divisor);
        if (!result.ok() || result.value >= 0) return result;
        return {(divisor > 0 ? result.value + divisor : result.value - divisor),
                StrictIntegerFailure::none};
    }

    // Manual signed bounds check avoids any overflowing multiplication.
    static constexpr bool multiply(I a, I b, I& out) noexcept {
        constexpr I lo = std::numeric_limits<I>::min();
        constexpr I hi = std::numeric_limits<I>::max();
        if (a > 0) {
            if (b > 0 && a > hi / b) return false;
            if (b < 0 && b < lo / a) return false;
        } else if (a < 0) {
            if (b > 0 && a < lo / b) return false;
            if (b < 0 && a < hi / b) return false;
        }
        out = a * b;
        return true;
    }

    static constexpr StrictIntegerResult pow(I base, I exponent) noexcept {
        if (exponent < 0) return error(StrictIntegerFailure::negative_exponent);
        auto remaining = static_cast<std::uint64_t>(exponent);
        I result = 1;
        while (remaining != 0) {
            if ((remaining & 1U) != 0U && !multiply(result, base, result)) {
                return error(StrictIntegerFailure::overflow);
            }
            remaining >>= 1U;
            if (remaining != 0 && !multiply(base, base, base)) {
                return error(StrictIntegerFailure::overflow);
            }
        }
        return {result, StrictIntegerFailure::none};
    }

    /**
     * Explicit lossless conversion to IEEE754 binary64. Check the upper
     * boundary before casting back to I: converting an out-of-range double
     * to signed integer has undefined behavior in native C++.
     */
    static constexpr StrictFloatResult toFloatExact(I value) noexcept {
        const double converted = static_cast<double>(value);
        if (converted >= 0x1.0p63 || converted < -0x1.0p63) {
            return {0.0, StrictIntegerFailure::inexact_conversion};
        }
        if (static_cast<I>(converted) != value) {
            return {0.0, StrictIntegerFailure::inexact_conversion};
        }
        return {converted, StrictIntegerFailure::none};
    }

    // Reconstruct an i64 from its unsigned two's-complement bit pattern
    // without implementation-defined uint64_t -> int64_t narrowing. Signed
    // inputs convert to uint64_t modulo 2^64, so the bitwise operation itself
    // has an exact, representation-independent 64-bit domain.
    static constexpr I fromUnsignedBits(std::uint64_t bits) noexcept {
        constexpr auto sign = std::uint64_t(1) << 63U;
        if ((bits & sign) == 0) return static_cast<I>(bits);
        const auto magnitude = (~bits) + std::uint64_t(1);
        if (magnitude == sign) return std::numeric_limits<I>::min();
        return -static_cast<I>(magnitude);
    }

    static constexpr StrictIntegerResult bitXor(I left, I right) noexcept {
        const auto bits = static_cast<std::uint64_t>(left)
            ^ static_cast<std::uint64_t>(right);
        return {fromUnsignedBits(bits), StrictIntegerFailure::none};
    }
};

} // namespace oreslang::runtime
