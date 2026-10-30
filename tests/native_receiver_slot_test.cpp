#include "oreslang/frontend/CallableSelector.hpp"
#include <cassert>
#include <stdexcept>
int main() {
  using namespace oreslang;
  assert(native_source_value_arity(1, true) == 0);
  assert(native_source_value_arity(2, true) == 1);
  assert(native_function_slot("x", 0) == "x$arity0");
  assert(native_function_slot("x", 1) == "x$arity1");
  assert(native_function_slot("x", 1) != native_function_slot("y", 1));
  bool failed = false;
  try { native_source_value_arity(0, true); }
  catch (const std::invalid_argument&) { failed = true; }
  assert(failed);
}
