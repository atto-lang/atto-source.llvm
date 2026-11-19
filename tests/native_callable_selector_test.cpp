#include "oreslang/frontend/CallableSelector.hpp"

#include <cassert>
#include <stdexcept>
#include <string>

using oreslang::native_function_slot;
using oreslang::native_source_value_arity;

int main() {
    assert(native_source_value_arity(0, false) == 0);
    assert(native_source_value_arity(2, false) == 2);
    assert(native_source_value_arity(1, true) == 0);
    assert(native_source_value_arity(2, true) == 1);
    assert(native_function_slot(
        "Box.read", native_source_value_arity(1, true)) == "Box.read$arity0");
    assert(native_function_slot(
        "Box.read", native_source_value_arity(2, true)) == "Box.read$arity1");

    bool rejected = false;
    try {
        (void)native_source_value_arity(0, true);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);
}
