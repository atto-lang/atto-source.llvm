#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace oreslang::dop {
class Plan;
struct GpuTarget;
struct OpenClArtifact;
}

namespace oreslang::pipeline_source { struct Plan; }

namespace oreslang {

// Deliberately restricted, typed-i64 expression AST. Not the full Java Ast.Program.
struct Expression {
    enum class Kind { Integer, Variable, Negate, Add, Subtract, Multiply, Call, Copy, Take, Share, Borrow, BorrowMut };
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
    Mode mode = Mode::Value;
};

// Immutable, lexical i64 slot with initializer-before-publication semantics.
struct LocalBinding {
    std::string name;
    std::unique_ptr<Expression> initializer;
    bool heap_owned = false;
    std::size_t offset = 0;
};

struct Function {
    std::string name;
    std::vector<Parameter> parameters;
    std::vector<LocalBinding> locals;
    std::unique_ptr<Expression> result;
    std::size_t offset = 0;
    bool exported = false;
};

struct Program {
    std::string module;
    std::vector<Function> functions;
};

// Explicit, trusted compiler-only accelerator admission. These are not
// LLVM guest-execution entry points. Source is lowered through the same
// fail-closed typed accelerator frontend and requires target profiles.
dop::OpenClArtifact compile_gpu_source(
        std::string_view source, dop::Plan layout, dop::GpuTarget target);
std::string compile_quantum_source_openqasm3(std::string_view source);

// Trusted source-compiled actor graph: only audited builtin roles and a pure
// f32 GPU transform are executable. Arbitrary actor methods remain rejected.
pipeline_source::Plan compile_pipeline_source(
        std::string_view source, dop::Plan layout, dop::GpuTarget target);

// Parse and statically validate a strict Java-compatible subset: int functions,
// int parameters, return expressions and same-module calls. Reject everything else.
Program parse_program(std::string_view source);

} // namespace oreslang
