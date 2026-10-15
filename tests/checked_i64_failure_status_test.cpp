// The typed effect identity must not be confused with a process exit,
// supervisor notice or a host exception.
#include "oreslang/runtime/CheckedI64Failure.hpp"
#include <cassert>
#include <string_view>

using namespace oreslang;
using oreslang::runtime::CheckedI64Failure;

int main() {
    static_assert(CheckedI64Failure::code == std::string_view("ORES_I64_OVERFLOW"));
    static_assert(CheckedI64Failure::guest_error_name ==
                  std::string_view("ArithmeticOverflowError"));
    static_assert(CheckedI64Failure::effect_code == 0x494F0001u);
    constexpr EffectStatus overflow = CheckedI64Failure::effect();
    static_assert(overflow.kind == EffectKind::Throw);
    static_assert(overflow.code == CheckedI64Failure::effect_code);
    static_assert(native_trap_admits(overflow.kind));
    static_assert(!effect_is_terminal(overflow.kind));
    using Operation = CheckedI64Failure::Operation;
    static_assert(CheckedI64Failure::valid_operation(1));
    static_assert(CheckedI64Failure::valid_operation(4));
    static_assert(!CheckedI64Failure::valid_operation(0));
    static_assert(!CheckedI64Failure::valid_operation(5));
    static_assert(CheckedI64Failure::operation_name(Operation::Add) ==
                  std::string_view("add"));
    static_assert(CheckedI64Failure::operation_name(Operation::Subtract) ==
                  std::string_view("subtract"));
    static_assert(CheckedI64Failure::operation_name(Operation::Multiply) ==
                  std::string_view("multiply"));
    static_assert(CheckedI64Failure::operation_name(Operation::Negate) ==
                  std::string_view("negate"));
    assert(CheckedI64Failure::is_checked_overflow(overflow));
    assert(!CheckedI64Failure::is_checked_overflow({EffectKind::Raise, overflow.code}));
    assert(!CheckedI64Failure::is_checked_overflow({EffectKind::Panic, overflow.code}));
    assert(!CheckedI64Failure::is_checked_overflow({EffectKind::Throw, 0}));
    assert(!CheckedI64Failure::is_checked_overflow({EffectKind::Throw, 1009}));
}
