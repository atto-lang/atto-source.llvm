// HOST-ONLY reference for Oreslang signed-i64 shift semantics.
// Not the production native LLVM Oreslang compiler or guest IR execution.
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>

namespace {
enum class Kind { Left, SignedRight, UnsignedRight };
std::uint64_t bits(std::int64_t value) {
    std::uint64_t result = 0;
    static_assert(sizeof(result) == sizeof(value));
    std::memcpy(&result, &value, sizeof(result));
    return result;
}
std::int64_t signed_bits(std::uint64_t value) {
    std::int64_t result = 0;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}
std::optional<std::int64_t> shift(Kind operation, std::int64_t value, std::int64_t distance) {
    if (distance < 0 || distance > 63) return std::nullopt;
    const auto count = static_cast<unsigned>(distance);
    const auto data = bits(value);
    if (operation == Kind::Left) return signed_bits(data << count);
    auto result = data >> count;
    if (operation == Kind::SignedRight && value < 0 && count != 0)
        result |= std::numeric_limits<std::uint64_t>::max() << (64 - count);
    return signed_bits(result);
}
void expect(bool valid, const char* what) {
    if (!valid) throw std::runtime_error(what);
}
} // namespace

int main() {
    constexpr auto MIN = std::numeric_limits<std::int64_t>::min();
    constexpr auto MAX = std::numeric_limits<std::int64_t>::max();
    expect(shift(Kind::SignedRight, 65535, 8) == 255, "ODB1 upper octet");
    expect(65535 - 256 * *shift(Kind::SignedRight, 65535, 8) == 255, "ODB1 lower octet");
    expect(shift(Kind::Left, 3, 3) == 24, "addition precedence");
    expect(shift(Kind::SignedRight, 256, 8) == 1, "right count precedence");
    expect(shift(Kind::SignedRight, *shift(Kind::SignedRight, 1024, 2), 2) == 64, "chained shift");
    expect(shift(Kind::SignedRight, -1, 1) == -1, "arithmetic sign extension");
    expect(shift(Kind::UnsignedRight, -1, 1) == MAX, "logical right sign cleared");
    expect(shift(Kind::UnsignedRight, MIN, 63) == 1, "minimum integer high bit");
    expect(shift(Kind::Left, 1, 63) == MIN, "signed left wrap semantics");
    expect(shift(Kind::UnsignedRight, -1, 0) == -1, "zero count preserves all bits");
    expect(shift(Kind::Left, 1, 0) == 1, "zero left count");
    expect(shift(Kind::SignedRight, MIN, 63) == -1, "minimum sign shift");
    const auto count = static_cast<std::int64_t>(8);
    expect(shift(Kind::SignedRight, 65535, count) == 255, "dynamic shift count");
    expect(!shift(Kind::SignedRight, 255, -1).has_value(), "negative count must reject");
    expect(!shift(Kind::Left, 1, 64).has_value(), "width count must reject");
    expect(!shift(Kind::UnsignedRight, -1, MAX).has_value(), "large count must reject");
    expect(!shift(Kind::SignedRight, 1, MIN).has_value(), "min count must reject");
    expect(shift(Kind::UnsignedRight, -1, 63) == 1, "rightmost sign bit");
    expect(shift(Kind::SignedRight, MIN, 0) == MIN, "minimum, no shift");
    std::cout << "HOST_ONLY_ODB1_SHIFT_ORACLE_PASS cases=19; NO ORESLANG GUEST COMPILER EXECUTION\n";
}
