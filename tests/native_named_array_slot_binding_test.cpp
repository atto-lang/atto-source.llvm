// Host-only structural admission check for the exact production native
// Array<T> slot binder. This does not substitute for compiler-generated
// LLVM IR, Clang linking, sanitizer or guest execution receipts.
#include "oreslang/frontend/Parser.hpp"

#include <cstddef>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using oreslang::Expression;
using oreslang::native_array_argument_slots;

Expression method(const std::string& name,
                  std::initializer_list<std::string> labels,
                  std::size_t arity) {
    Expression call{};
    call.kind = Expression::Kind::ArrayCall;
    call.name = name;
    call.argument_names.assign(labels.begin(), labels.end());
    for (std::size_t i = 0; i < arity; ++i) {
        auto argument = std::make_unique<Expression>();
        argument->kind = Expression::Kind::Integer;
        argument->integer = static_cast<std::int64_t>(i);
        call.arguments.push_back(std::move(argument));
    }
    return call;
}

void accepted(const std::string& name,
              std::initializer_list<std::string> labels,
              std::size_t arity,
              const std::vector<std::size_t>& expected) {
    const auto call = method(name, labels, arity);
    const auto actual = native_array_argument_slots(call);
    if (actual != expected) {
        throw std::runtime_error("unexpected native array ABI slot order for " + name);
    }
}

void rejected(const std::string& name,
              std::initializer_list<std::string> labels,
              std::size_t arity,
              const std::string& diagnostic) {
    const auto call = method(name, labels, arity);
    try {
        (void)native_array_argument_slots(call);
    } catch (const std::runtime_error& error) {
        if (std::string(error.what()).find(diagnostic) != std::string::npos)
            return;
        throw std::runtime_error("unexpected array binder rejection: " +
                                 std::string(error.what()));
    }
    throw std::runtime_error("named native Array method admitted invalid arguments");
}
}  // namespace

int main() {
    try {
        accepted("get", {"index"}, 1, {0});
        accepted("get", {""}, 1, {0});
        accepted("add", {"value"}, 1, {0});
        accepted("add", {""}, 1, {0});
        accepted("set", {"index", "value"}, 2, {0, 1});
        accepted("set", {"value", "index"}, 2, {1, 0});
        accepted("set", {"", "value"}, 2, {0, 1});
        accepted("set", {"", ""}, 2, {0, 1});
        accepted("set", {}, 2, {0, 1});
        rejected("get", {"value"}, 1, "unknown named native array argument");
        rejected("add", {"index"}, 1, "unknown named native array argument");
        rejected("set", {"value", "Value"}, 2, "unknown named native array argument");
        rejected("set", {"index", "index"}, 2, "duplicate native array argument binding");
        rejected("set", {"value", "value"}, 2, "duplicate native array argument binding");
        rejected("set", {"", "index"}, 2, "duplicate native array argument binding");
        rejected("set", {"index", ""}, 2, "positional member argument cannot follow named argument");
        rejected("set", {"value"}, 1, "native Array method argument arity mismatch");
        rejected("set", {"index", "value", "extra"}, 3, "native Array method argument arity mismatch");
        rejected("set", {"index"}, 2, "invalid native member argument layout");
        rejected("remove", {"index"}, 1, "supports only add(value) or set(index,value)");
        rejected("get", {"Index"}, 1, "unknown named native array argument");
    } catch (const std::exception& error) {
        std::cerr << "native named Array slot binding FAILED: " << error.what() << '\n';
        return 1;
    }
    std::cout << "native named Array slot binding: 21 cases passed (host-only)" << '\n';
    return 0;
}
