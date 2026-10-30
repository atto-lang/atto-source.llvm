#include "oreslang/frontend/CallableSelector.hpp"
#include <iostream>
#include <stdexcept>

namespace {

bool check(bool condition, const char* message) {
  if (condition) return true;
  std::cerr << "FAIL native receiver slot contract: " << message << '\n';
  return false;
}

} // namespace

int main() {
  using namespace oreslang;

  bool ok = true;
  ok &= check(native_source_value_arity(0, false) == 0,
              "plain zero-arity function changed arity");
  ok &= check(native_source_value_arity(2, false) == 2,
              "plain function arity must not erase a receiver");
  ok &= check(native_source_value_arity(1, true) == 0,
              "hidden receiver must erase from zero-value-arity method");
  ok &= check(native_source_value_arity(2, true) == 1,
              "hidden receiver must erase from one-value-arity method");

  ok &= check(native_function_slot("x", 0) == "x$arity0",
              "zero-arity selector encoding drifted");
  ok &= check(native_function_slot("x", 1) == "x$arity1",
              "one-arity selector encoding drifted");
  ok &= check(native_function_slot("x", 1) != native_function_slot("y", 1),
              "selector must retain callable name");

  bool rejected_missing_receiver = false;
  try {
    (void)native_source_value_arity(0, true);
  } catch (const std::invalid_argument&) {
    rejected_missing_receiver = true;
  }
  ok &= check(rejected_missing_receiver,
              "lowered method without hidden receiver was accepted");

  return ok ? 0 : 1;
}
