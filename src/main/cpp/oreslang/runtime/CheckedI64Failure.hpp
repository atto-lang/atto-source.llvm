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

    // Stable operation discriminants; values/operands never cross this ABI.
    enum class Operation : std::uint32_t {
        Add = 1u, Subtract = 2u, Multiply = 3u, Negate = 4u
    };
    // Internal host-scalar checked invocation status: upper 32 bits carry
    // operation identity; lower 32 carry the Throw error code. Zero means
    // success. This is not a source Error object or serialized guest ABI.
    [[nodiscard]] static constexpr std::uint64_t packed_status(Operation op) noexcept {
        return (std::uint64_t(static_cast<std::uint32_t>(op)) << 32) | effect_code;
    }
    [[nodiscard]] static constexpr bool valid_packed_status(std::uint64_t bits) noexcept {
        return (bits & 0xFFFFFFFFull) == effect_code &&
               valid_operation(static_cast<std::uint32_t>(bits >> 32));
    }
    [[nodiscard]] static constexpr bool valid_operation(std::uint32_t op) noexcept {
        return op >= static_cast<std::uint32_t>(Operation::Add) &&
               op <= static_cast<std::uint32_t>(Operation::Negate);
    }
    [[nodiscard]] static constexpr std::string_view operation_name(Operation op) noexcept {
        switch (op) {
            case Operation::Add: return "add";
            case Operation::Subtract: return "subtract";
            case Operation::Multiply: return "multiply";
            case Operation::Negate: return "negate";
        }
        return {};
    }

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
