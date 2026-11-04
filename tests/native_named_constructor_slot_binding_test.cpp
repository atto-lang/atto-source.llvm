// Standalone host-only test against the actual Parser.hpp constructor binder.
// Full native compiler/Clang guest receipts are separate CTest gates.
#include "oreslang/frontend/Parser.hpp"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using oreslang::ClassDecl;
using oreslang::Expression;
using oreslang::LocalBinding;
using oreslang::native_constructor_argument_slots;

ClassDecl cls(const std::string& name,
              std::initializer_list<std::string> formals) {
    ClassDecl d;
    d.name = name;
    d.constructor_parameters.assign(formals.begin(), formals.end());
    return d;
}

LocalBinding call(const std::string& name,
                  std::initializer_list<std::string> names,
                  std::size_t actuals) {
    LocalBinding b;
    b.new_class = true;
    b.class_name = name;
    b.constructor_argument_names.assign(names.begin(), names.end());
    for (std::size_t i = 0; i < actuals; ++i) {
        auto value = std::make_unique<Expression>();
        value->kind = Expression::Kind::Integer;
        value->integer = static_cast<std::int64_t>(i);
        b.constructor_arguments.push_back(std::move(value));
    }
    return b;
}

void accepted(const ClassDecl& definition,
              const std::string& type,
              std::initializer_list<std::string> names,
              std::size_t actuals,
              const std::vector<std::size_t>& expected) {
    const auto binding = call(type, names, actuals);
    const auto mapping = native_constructor_argument_slots(binding, definition);
    if (mapping != expected)
        throw std::runtime_error("native constructor slot permutation mismatch");
}

void rejected(const ClassDecl& definition,
              const std::string& type,
              std::initializer_list<std::string> names,
              std::size_t actuals,
              const std::string& expected) {
    const auto binding = call(type, names, actuals);
    try {
        (void)native_constructor_argument_slots(binding, definition);
    } catch (const std::runtime_error& error) {
        if (std::string(error.what()).find(expected) != std::string::npos)
            return;
        throw std::runtime_error(
            "wrong native constructor error: " + std::string(error.what()));
    }
    throw std::runtime_error("invalid native constructor call was accepted");
}
}  // namespace

int main() {
    try {
        const auto pair = cls("Pair", {"first", "second"});
        const auto triple = cls("Triple", {"first", "second", "third"});
        const auto empty = cls("Empty", {});
        const auto single = cls("Single", {"value"});
        accepted(empty, "Empty", {}, 0, {});
        accepted(pair, "Pair", {}, 2, {0, 1});
        accepted(pair, "Pair", {"", ""}, 2, {0, 1});
        accepted(pair, "Pair", {"first", "second"}, 2, {0, 1});
        accepted(pair, "Pair", {"second", "first"}, 2, {1, 0});
        accepted(pair, "Pair", {"", "second"}, 2, {0, 1});
        accepted(single, "Single", {"value"}, 1, {0});
        accepted(single, "Single", {""}, 1, {0});
        accepted(triple, "Triple", {"third", "first", "second"}, 3, {2, 0, 1});
        accepted(triple, "Triple", {"", "third", "second"}, 3, {0, 2, 1});

        rejected(pair, "Pair", {"missing", "second"}, 2,
                 "unknown named native constructor argument");
        rejected(pair, "Pair", {"First", "second"}, 2,
                 "unknown named native constructor argument");
        rejected(pair, "Pair", {"first", "first"}, 2,
                 "duplicate native constructor argument binding");
        rejected(pair, "Pair", {"", "first"}, 2,
                 "duplicate native constructor argument binding");
        rejected(pair, "Pair", {"second", ""}, 2,
                 "positional constructor argument cannot follow named argument");
        rejected(pair, "Pair", {"first"}, 1,
                 "native class constructor argument arity mismatch");
        rejected(pair, "Pair", {"first", "second", "extra"}, 3,
                 "native class constructor argument arity mismatch");
        rejected(pair, "Pair", {"first"}, 2,
                 "invalid native constructor label layout");
        rejected(pair, "Other", {"first", "second"}, 2,
                 "native constructor binding requires exact class");
    } catch (const std::exception& e) {
        std::cerr << "named native constructor binder FAILED: " << e.what() << '\n';
        return 1;
    }
    std::cout << "named native constructor binder: 19 host-only cases passed" << '\n';
    return 0;
}
