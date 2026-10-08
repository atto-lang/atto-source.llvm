#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace oreslang {

// The first *supported* AST slice; it is not the full Java Ast.Program.
struct ConstantFunction {
    std::string module;
    std::string name;
    std::int64_t value = 0;
    bool exported = false;
};

// Parse exactly one zero-argument fnc/routine with an integer literal return.
// Unexpected language features fail closed with a source-offset diagnostic.
ConstantFunction parse_constant_function(std::string_view source);

} // namespace oreslang
