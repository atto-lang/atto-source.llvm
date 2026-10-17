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

int main() {
    if (!oreslang_bool_yes() || oreslang_bool_no() ||
            oreslang_bool_and_word() || !oreslang_bool_and_symbol() ||
            !oreslang_bool_or_word() || oreslang_bool_or_symbol() ||
            !oreslang_bool_xor_symbol() || !oreslang_bool_nested())
        throw std::runtime_error("compiled native i1 logical result mismatch");
}
