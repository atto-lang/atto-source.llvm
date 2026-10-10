#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace oreslang {

// Deliberately restricted, typed-i64 expression AST. Not the full Java Ast.Program.
struct Expression {
    enum class Kind { Integer, Variable, Negate, Add, Subtract, Multiply, Call };
    Kind kind;
    std::size_t offset = 0;
    std::int64_t integer = 0;
    std::string name;
    std::unique_ptr<Expression> left;
    std::unique_ptr<Expression> right;
    std::vector<std::unique_ptr<Expression>> arguments;
};

// A trivially owned, immutable i64 local. The first LLVM lexical-storage
// lowering scope has deterministic entry/exit, with no heap or GC collector.
struct LocalBinding {
    std::string name;
    std::unique_ptr<Expression> initializer;
    bool heap_owned = false;
    std::size_t offset = 0;
};

struct Function {
    std::string name;
    std::vector<std::string> parameters;
    std::vector<LocalBinding> locals;
    std::unique_ptr<Expression> result;
    std::size_t offset = 0;
    bool exported = false;
};

struct Actor {
    std::string name;
    std::vector<LocalBinding> fields;
    std::vector<Function> methods;
    std::size_t offset = 0;
};

struct Program {
    std::string module;
    std::vector<Function> functions;
    std::vector<Actor> actors;
};

// Parse and statically validate a strict Java-compatible subset: int functions,
// int parameters, return expressions and same-module calls. Reject everything else.
Program parse_program(std::string_view source);

} // namespace oreslang
