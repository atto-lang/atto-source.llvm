#include <stdexcept>
// Invoke real compiled Oreslang exports, not a reimplementation in C++.
extern "C" bool oreslang_bool_yes() asm("BoolProbe.yes");
extern "C" bool oreslang_bool_no() asm("BoolProbe.no");
extern "C" bool oreslang_bool_and_word() asm("BoolProbe.and_word");
extern "C" bool oreslang_bool_and_symbol() asm("BoolProbe.and_symbol");
extern "C" bool oreslang_bool_or_word() asm("BoolProbe.or_word");
extern "C" bool oreslang_bool_or_symbol() asm("BoolProbe.or_symbol");
extern "C" bool oreslang_bool_xor_symbol() asm("BoolProbe.xor_symbol");
extern "C" bool oreslang_bool_nested() asm("BoolProbe.nested");
extern "C" bool oreslang_bool_and_args(bool, bool) asm("BoolProbe.and_args");
extern "C" bool oreslang_bool_or_args(bool, bool) asm("BoolProbe.or_args");
extern "C" bool oreslang_bool_xor_args(bool, bool) asm("BoolProbe.xor_args");
extern "C" bool oreslang_bool_builtin_and(bool, bool, bool) asm("BoolProbe.builtin_and");
extern "C" bool oreslang_bool_builtin_or(bool, bool, bool) asm("BoolProbe.builtin_or");
extern "C" bool oreslang_bool_builtin_xor(bool, bool) asm("BoolProbe.builtin_xor");
extern "C" bool oreslang_bool_forward(bool, bool) asm("BoolProbe.forward");
extern "C" bool oreslang_bool_named_forward(bool, bool) asm("BoolProbe.named_forward");
extern "C" bool oreslang_bool_compose(bool, bool, bool) asm("BoolProbe.compose");
extern "C" bool oreslang_bool_builtin_and8(
    bool, bool, bool, bool, bool, bool, bool, bool) asm("BoolProbe.builtin_and8");
extern "C" bool oreslang_bool_builtin_or8(
    bool, bool, bool, bool, bool, bool, bool, bool) asm("BoolProbe.builtin_or8");
extern "C" bool oreslang_bool_precedence_probe() asm("BoolProbe.precedence_probe");
extern "C" bool oreslang_bool_lazy_and_call(bool, bool)
    asm("BoolProbe.lazy_and_call");
extern "C" bool oreslang_bool_lazy_or_call(bool, bool)
    asm("BoolProbe.lazy_or_call");
extern "C" bool oreslang_bool_eager_xor_calls(bool, bool)
    asm("BoolProbe.eager_xor_calls");

int main() {
    if (!oreslang_bool_yes() || oreslang_bool_no() ||
            oreslang_bool_and_word() || !oreslang_bool_and_symbol() ||
            !oreslang_bool_or_word() || oreslang_bool_or_symbol() ||
            !oreslang_bool_xor_symbol() || !oreslang_bool_nested() ||
            !oreslang_bool_and_args(true, true) ||
            oreslang_bool_and_args(true, false) ||
            !oreslang_bool_or_args(false, true) ||
            oreslang_bool_or_args(false, false) ||
            !oreslang_bool_xor_args(true, false) ||
            oreslang_bool_xor_args(true, true) ||
            !oreslang_bool_builtin_and(true, true, true) ||
            oreslang_bool_builtin_and(true, false, true) ||
            !oreslang_bool_builtin_or(false, false, true) ||
            oreslang_bool_builtin_or(false, false, false) ||
            !oreslang_bool_builtin_xor(true, false) ||
            oreslang_bool_builtin_xor(true, true) ||
            !oreslang_bool_forward(true, true) ||
            oreslang_bool_forward(true, false) ||
            !oreslang_bool_named_forward(true, true) ||
            oreslang_bool_named_forward(true, false) ||
            !oreslang_bool_compose(true, true, false) ||
            oreslang_bool_compose(false, false, false) ||
            !oreslang_bool_builtin_and8(
                true, true, true, true, true, true, true, true) ||
            oreslang_bool_builtin_and8(
                true, true, true, false, true, true, true, true) ||
            !oreslang_bool_builtin_or8(
                false, false, false, false, false, false, false, true) ||
            oreslang_bool_builtin_or8(
                false, false, false, false, false, false, false, false) ||
            !oreslang_bool_precedence_probe() ||
            oreslang_bool_lazy_and_call(false, true) ||
            !oreslang_bool_lazy_and_call(true, true) ||
            !oreslang_bool_lazy_or_call(true, false) ||
            oreslang_bool_lazy_or_call(false, false) ||
            !oreslang_bool_eager_xor_calls(true, false) ||
            oreslang_bool_eager_xor_calls(true, true))
        throw std::runtime_error("compiled native i1 Boolean graph mismatch");
}
