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
    return 0;
}
