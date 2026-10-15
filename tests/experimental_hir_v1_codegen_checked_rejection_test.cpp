// Full-LLVM backend regression: checked-i64 may never bypass overflow checks
// through special-case native code generation paths.
#include "oreslang/backend/LlvmCodegen.hpp"
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

using namespace oreslang;

static void expect_checked_rejection(const Program& program,
                                     bool wasm = false, bool cortex = false) {
    try {
        (void) emit_verified_llvm_ir(program, wasm, cortex, true);
    } catch (const std::runtime_error& failure) {
        const std::string message(failure.what());
        if (message.find("checked-i64-v1") != std::string::npos)
            return;
        throw;
    }
    throw std::runtime_error("checked-i64 unexpectedly accepted unsupported lowering");
}

static Program valid_scalar_program() {
    Program program;
    Function function;
    function.name = "main";
    function.result = std::make_unique<Expression>();
    function.result->kind = Expression::Kind::Integer;
    function.result->integer = 43;
    program.functions.push_back(std::move(function));
    return program;
}

int main() {
    // The scalar-while emitter exits before a normal LLVM module exists.
    // Missing this guard would silently generate unchecked integer operations.
    Program scalar;
    Function loop;
    loop.name = "main";
    loop.scalar_while = std::make_unique<ScalarWhileLoop>();
    scalar.functions.push_back(std::move(loop));
    expect_checked_rejection(scalar);

    Program cooperative;
    cooperative.cooperative_loops.emplace_back();
    expect_checked_rejection(cooperative);

    Program actors;
    actors.actors.emplace_back();
    expect_checked_rejection(actors);

    Program classes;
    classes.classes.emplace_back();
    expect_checked_rejection(classes);

    Program minimal;
    expect_checked_rejection(minimal, true, false);
    expect_checked_rejection(minimal, false, true);
    // A valid scalar program provides the baseline; each mutation isolates
    // one escape from the checked host profile.
    (void) emit_verified_llvm_ir(valid_scalar_program(), false, false, true);

    Program strayCatchBindingRejects = valid_scalar_program();
    strayCatchBindingRejects.functions[0].catch_binding = "caught";
    expect_checked_rejection(strayCatchBindingRejects);

    Program caughtThrow = valid_scalar_program();
    caughtThrow.functions[0].caught_throw_value = std::make_unique<Expression>();
    caughtThrow.functions[0].caught_throw_value->kind = Expression::Kind::Integer;
    expect_checked_rejection(caughtThrow);

    Program effects = valid_scalar_program();
    effects.functions[0].has_declared_effects = true;
    expect_checked_rejection(effects);

    Program statements = valid_scalar_program();
    statements.functions[0].statements.emplace_back();
    expect_checked_rejection(statements);

    Program locals = valid_scalar_program();
    locals.functions[0].locals.emplace_back();
    expect_checked_rejection(locals);

    Program recursive = valid_scalar_program();
    recursive.functions[0].result->kind = Expression::Kind::Call;
    recursive.functions[0].result->name = "main";
    expect_checked_rejection(recursive);

    return 0;
}
