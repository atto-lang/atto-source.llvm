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
    enum class Kind { Integer, Variable, Field, Negate, Add, Subtract, Multiply, Call };
    Kind kind;
    std::size_t offset = 0;
    std::int64_t integer = 0;
    std::string name;
    std::unique_ptr<Expression> left;
    std::unique_ptr<Expression> right;
    std::vector<std::unique_ptr<Expression>> arguments;
};

// Compile-time-known, non-escaping, stack-allocated class objects only.
// The full dynamically allocated/borrowed Oreslang object model is separate.
struct ClassField {
    std::string name;
    std::int64_t initial = 0;
};
struct PlainClass {
    std::string name;
    std::vector<ClassField> fields;
};
struct StackObject {
    std::string type;
    std::string name;
};

struct Function {
    std::string name;
    std::vector<std::string> parameters;
    std::vector<StackObject> stack_objects;
    std::unique_ptr<Expression> result;
    std::size_t offset = 0;
    bool exported = false;
};

struct Program {
    std::string module;
    std::vector<PlainClass> plain_classes;
    std::vector<Function> functions;
};

// Parse and validate a closed subset: int arithmetic/functions and nonescaping,
// integer-field-only stack-local class instances. Reject heap escaping and all
// unsupported ownership operations rather than emitting unsafe LLVM IR.
Program parse_program(std::string_view source);

} // namespace oreslang
