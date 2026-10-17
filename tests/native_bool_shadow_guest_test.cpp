#include <stdexcept>

extern "C" bool oreslang_bool_shadow_probe(bool, bool)
    asm("BoolShadow.probe");

int main() {
    if (oreslang_bool_shadow_probe(false, true) ||
            !oreslang_bool_shadow_probe(true, false))
        throw std::runtime_error(
            "user-defined And was replaced by compiler intrinsic");
}
