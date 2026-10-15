// Independent, compiler-executed property tests for the conservative HIR
// interval endpoint arithmetic. This does NOT test LLVM-generated guest code.
#include "oreslang/frontend/ExperimentalHirV1.hpp"
#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>

int main() {
    using i64 = std::int64_t;
    const i64 min = std::numeric_limits<i64>::min();
    const i64 max = std::numeric_limits<i64>::max();
    const std::array<i64, 11> edges =
        {min, min + 1, -43, -2, -1, 0, 1, 2, 43, max - 1, max};
    std::uint64_t examined = 0;

    auto check = [&](i64 left, i64 right, char op) {
        // Widen BEFORE arithmetic, avoiding undefined host signed overflow.
        const __int128 a = static_cast<__int128>(left);
        const __int128 b = static_cast<__int128>(right);
        const __int128 math = op == '+' ? a + b : op == '-' ? a - b : a * b;
        const bool should_succeed =
            math >= static_cast<__int128>(min) &&
            math <= static_cast<__int128>(max);
        bool succeeded = false;
        i64 value = 0;
        try {
            value = oreslang::hir_v1_safe_arithmetic(left, right, op);
            succeeded = true;
        } catch (const std::runtime_error&) { }
        assert(succeeded == should_succeed);
        if (should_succeed) assert(static_cast<__int128>(value) == math);
        ++examined;
    };

    for (i64 left : edges)
        for (i64 right : edges)
            for (char op : {'+', '-', '*'}) check(left, right, op);

    std::mt19937_64 rng(20261014);
    for (int i = 0; i < 8192; ++i) {
        // unsigned-to-signed mapping without implementation-defined cast.
        const auto signed_draw = [&]() -> i64 {
            const std::uint64_t raw = rng();
            const __int128 shifted = static_cast<__int128>(raw) -
                                     (static_cast<__int128>(1) << 63);
            return static_cast<i64>(shifted);
        };
        const i64 a = signed_draw(), b = signed_draw();
        for (char op : {'+', '-', '*'}) check(a, b, op);
    }
    std::cout << "checked i64 endpoint property: " << examined
              << " operations passed\n";
}
