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
    enum class Kind { Integer, Variable, Negate, Add, Subtract, Multiply, Call, ActorField };
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

// Restricted actor field fragment: i64 fields and serial methods. No refs,
// await, exceptions or constructor callbacks enter this grammar.
struct ActorField {
    bool mutable_field = false;
    std::string name;
    std::unique_ptr<Expression> initializer;
    std::size_t offset = 0;
};
struct ActorAssignment {
    std::string field;
    std::unique_ptr<Expression> value;
    std::size_t offset = 0;
};
struct ActorMethod {
    std::string name;
    std::vector<std::string> parameters;
    std::vector<ActorAssignment> assignments;
    std::unique_ptr<Expression> result;
    std::string accepted_message_field;
    bool ends_after_receive = false;
    std::size_t offset = 0;
};
struct ActorDecl {
    std::string name;
    std::vector<ActorField> fields;
    std::vector<ActorMethod> methods;
    std::size_t offset = 0;
};

// Restricted source-level actor driver. The compiler produces a yielding
// native continuation for spawn/ready/typed send/done (NO guest method calls),
// rather than a blocking thread-per-await. No general actor references,
// nested function calls or source FFI escapes are supported yet.
struct ActorScript {
    std::string actor_name;
    std::string worker_name;
    std::string method; // always the actor-local receive handler
    std::string message_field;
    std::unique_ptr<Expression> argument; // structurally typed message field
    bool await_spawn = false;
    std::size_t offset = 0;
};

struct Program {
    std::unique_ptr<ActorDecl> actor;
    std::unique_ptr<ActorScript> script;
    std::string module;
    std::vector<Function> functions;
};

// Parse and validate the strict i64 function and serial actor-field fragments.
// Source cannot manufacture actor heap, turn epochs, or field-table pointers.
Program parse_program(std::string_view source);

} // namespace oreslang
