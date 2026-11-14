#pragma once

#include "oreslang/runtime/EffectUnwind.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace oreslang::assert_scalar_abi {

// ABI v0: primitive-only staging for native LLVM lowering. This is not the
// whole Any layout, and does not implement source-owned std/assert linking.
// Heap/String/actor/reference values are deliberately excluded until their
// ownership and equality semantics have a separately tested native ABI.
enum class ScalarTag : std::uint8_t {
    I64 = 1,
    Boolean = 2
};

// Explicit 16-byte, host-endian scalar envelope. A real IR lowering must
// generate and verify this exact layout instead of assuming an LLVM i64 is
// interchangeable with a source-language Any or bool.
struct alignas(8) Scalar final {
    std::uint64_t bits = 0;
    std::uint8_t tag = 0;
    std::array<std::uint8_t, 7> reserved{};
};

// The effect and Option<bool> are independent tagged channels. None is
// (Return, present=0), and an ordinary source Throw is NOT itself None.
// Only the trap boundary may convert a Throw into None.
struct alignas(8) BoolResult final {
    std::uint32_t code = 0;
    std::uint8_t effect = static_cast<std::uint8_t>(EffectKind::Return);
    std::uint8_t present = 0;
    std::uint8_t value = 0;
    std::uint8_t reserved = 0;
};

static_assert(std::is_standard_layout_v<Scalar> &&
              std::is_trivially_copyable_v<Scalar>);
static_assert(sizeof(Scalar) == 16 && alignof(Scalar) == 8);
static_assert(offsetof(Scalar, bits) == 0 && offsetof(Scalar, tag) == 8);
static_assert(offsetof(Scalar, reserved) == 9);
static_assert(std::is_standard_layout_v<BoolResult> &&
              std::is_trivially_copyable_v<BoolResult>);
static_assert(sizeof(BoolResult) == 8 && alignof(BoolResult) == 8);
static_assert(offsetof(BoolResult, code) == 0 &&
              offsetof(BoolResult, effect) == 4 &&
              offsetof(BoolResult, present) == 5 &&
              offsetof(BoolResult, value) == 6 &&
              offsetof(BoolResult, reserved) == 7);

constexpr Scalar i64(std::int64_t value) noexcept {
    return {static_cast<std::uint64_t>(value),
            static_cast<std::uint8_t>(ScalarTag::I64), {}};
}

constexpr Scalar boolean(bool value) noexcept {
    return {value ? 1u : 0u,
            static_cast<std::uint8_t>(ScalarTag::Boolean), {}};
}

constexpr bool valid(Scalar value) noexcept {
    for (const std::uint8_t byte : value.reserved)
        if (byte != 0) return false;
    switch (static_cast<ScalarTag>(value.tag)) {
        case ScalarTag::I64: return true;
        case ScalarTag::Boolean: return value.bits <= 1;
    }
    return false;
}

constexpr bool valid(BoolResult result) noexcept {
    if (result.reserved != 0 || result.present > 1 || result.value > 1)
        return false;
    const auto kind = static_cast<EffectKind>(result.effect);
    if (!effect_kind_valid(kind)) return false;
    if (kind != EffectKind::Return && (result.present || result.value))
        return false;
    if (result.present == 0 && result.value != 0) return false;
    if (kind == EffectKind::Return && result.code != 0) return false;
    return true;
}

constexpr BoolResult malformed() noexcept {
    return {0xFFFFFFFDu, static_cast<std::uint8_t>(EffectKind::Fatal),
            0, 0, 0};
}
constexpr BoolResult some(bool value) noexcept {
    return {0, static_cast<std::uint8_t>(EffectKind::Return),
            1, static_cast<std::uint8_t>(value ? 1u : 0u), 0};
}
constexpr BoolResult none() noexcept {
    return {0, static_cast<std::uint8_t>(EffectKind::Return), 0, 0, 0};
}
constexpr BoolResult effect(EffectKind kind, std::uint32_t code) noexcept {
    if (!effect_kind_valid(kind)) return malformed();
    if (kind == EffectKind::Return)
        return code == 0 ? none() : malformed();
    return {code, static_cast<std::uint8_t>(kind), 0, 0, 0};
}

// Pure primitive semantic model, NOT an implementation of imported .ores
// Assert. This gives LLVM scalar lowering an explicit, adversarially tested
// typed contract for equal and equal_throws before source module execution.
constexpr BoolResult equal(Scalar lhs, Scalar rhs,
                           bool throws_on_false = false) noexcept {
    if (!valid(lhs) || !valid(rhs)) return malformed();
    const bool same = lhs.tag == rhs.tag && lhs.bits == rhs.bits;
    if (throws_on_false && !same)
        return effect(EffectKind::Throw, 1);
    return some(same);
}

constexpr BoolResult trap(BoolResult input) noexcept {
    if (!valid(input)) return malformed();
    if (static_cast<EffectKind>(input.effect) == EffectKind::Throw)
        return none();
    return input; // Raise, Panic, Cancel, Fatal, ProcessExit propagate.
}

} // namespace oreslang::assert_scalar_abi
