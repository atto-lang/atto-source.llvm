#pragma once

#include "oreslang/runtime/EffectUnwind.hpp"
#include <cstdint>
#include <string_view>

namespace oreslang::runtime {

// Versioned checked-i64 guest error identity shared by the experimental
// common-source contract and the Java checked-i64 host boundary. This data
// representation alone does not implement LLVM unwinding or actor dispatch.
struct CheckedI64Failure final {
    static constexpr std::string_view guest_error_name = "ArithmeticOverflowError";
    static constexpr std::string_view code = "ORES_I64_OVERFLOW";
    static constexpr std::uint32_t effect_code = 0x494F0001u;

    [[nodiscard]] static constexpr EffectStatus effect() noexcept {
        return {EffectKind::Throw, effect_code};
    }

    [[nodiscard]] static constexpr bool is_checked_overflow(EffectStatus status) noexcept {
        return status.kind == EffectKind::Throw && status.code == effect_code;
    }
};

static_assert(native_trap_admits(CheckedI64Failure::effect().kind));
static_assert(!effect_is_terminal(CheckedI64Failure::effect().kind));

} // namespace oreslang::runtime
