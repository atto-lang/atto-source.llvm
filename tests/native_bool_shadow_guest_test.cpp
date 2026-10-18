#include <stdexcept>

extern "C" bool oreslang_bool_shadow_unqualified(bool, bool)
    asm("BooleanOps.probe_unqualified");
extern "C" bool oreslang_bool_shadow_qualified(bool, bool)
    asm("BooleanOps.probe_qualified");

int main() {
    for (auto probe : {oreslang_bool_shadow_unqualified,
                       oreslang_bool_shadow_qualified}) {
        if (probe(false, true) || !probe(true, false))
            throw std::runtime_error(
                "user-defined BooleanOps.And was replaced by compiler intrinsic");
    }
}
