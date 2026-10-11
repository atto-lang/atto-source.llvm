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
    enum class Kind { Integer, Variable, Negate, Add, Subtract, Multiply, Call, Copy, Take, Share, Borrow, BorrowMut, Field };
    Kind kind;
    std::size_t offset = 0;
    std::int64_t integer = 0;
    std::string name;
    std::unique_ptr<Expression> left;
    std::unique_ptr<Expression> right;
    std::vector<std::unique_ptr<Expression>> arguments;
};

struct Parameter {
    enum class Mode { Value, Borrow, BorrowMut };
    std::string name;
    std::string class_name; // empty means primitive i64
    Mode mode = Mode::Value;
};

// Immutable, lexical i64 slot with initializer-before-publication semantics.
struct LocalBinding {
    std::string name;
    std::unique_ptr<Expression> initializer;
    bool heap_owned = false;
    std::string class_name; // only stack-constructed, bounded one-field class
    bool new_class = false;
    std::size_t offset = 0;
};

struct ClassDecl {
    std::string name;
    std::string field;
    std::int64_t initial = 0;
};

struct Statement {
    enum class Kind { Call, FieldWrite };
    Kind kind;
    std::unique_ptr<Expression> expression;
    std::string receiver;
    std::string field;
    std::size_t offset = 0;
};

struct Function {
    std::string name;
    std::vector<Parameter> parameters;
    std::vector<LocalBinding> locals;
    std::vector<Statement> statements;
    bool returns_void = false;
    std::unique_ptr<Expression> result;
    std::size_t offset = 0;
    bool exported = false;
};

struct Program {
    std::string module;
    std::vector<ClassDecl> classes;
    std::vector<Function> functions;
};

// Parse a constrained native subset: i64 expressions, call-scoped borrows and
// exactly one-field i64 classes. No escaping class pointers, GC, or suspension.
Program parse_program(std::string_view source);

} // namespace oreslang
