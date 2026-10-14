#include <stdexcept>
// Invoke real compiled Oreslang exports, not a reimplementation in C++.
extern "C" bool oreslang_bool_yes() asm("BoolProbe.yes");
extern "C" bool oreslang_bool_no() asm("BoolProbe.no");
int main() {
    if (!oreslang_bool_yes() || oreslang_bool_no())
        throw std::runtime_error("compiled native i1 bool result mismatch");
}
