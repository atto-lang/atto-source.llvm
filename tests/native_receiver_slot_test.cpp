#include "oreslang/frontend/CallableSelector.hpp"
#include <iostream>
#include <limits>
#include <stdexcept>
#include <unordered_set>

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

  // Bounded exhaustive sweep: erasing hidden self never changes a
  // caller-visible slot, and distinct arities/names retain distinct keys.
  std::unordered_set<std::string> observed_slots;
  for (std::size_t arity = 0; arity <= 1024; ++arity) {
    ok &= check(native_source_value_arity(arity, false) == arity,
                "free function arity was unexpectedly changed");
    ok &= check(native_source_value_arity(arity + 1, true) == arity,
                "hidden instance receiver counted toward value arity");
    const auto slot = native_function_slot("x", arity);
    ok &= check(observed_slots.insert(slot).second,
                "two different value arities collided in the selector");
    ok &= check(slot == native_function_slot("x", native_source_value_arity(arity + 1, true)),
                "explicit self spelling changed the method selector");
    ok &= check(slot != native_function_slot("y", arity),
                "different method names collided at same arity");
  }
  ok &= check(observed_slots.size() == 1025,
              "receiver slot sweep lost an arity");
  const auto highest = std::numeric_limits<std::size_t>::max();
  ok &= check(native_source_value_arity(highest, true) == highest - 1,
              "max lowered arity was not erased exactly once");
  ok &= check(native_function_slot("x", highest) != native_function_slot("x", highest - 1),
              "max arity selector collided with its predecessor");

  return ok ? 0 : 1;
}
