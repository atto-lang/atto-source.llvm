#include "oreslang/backend/LlvmCodegen.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

using oreslang::Expression;
using oreslang::Function;
using oreslang::Program;

static Program program(std::string name, bool boolean = false) {
    Program source;
    Function fn;
    fn.name = std::move(name);
    fn.returns_bool = boolean;
    fn.result = std::make_unique<Expression>();
    fn.result->kind = boolean ? Expression::Kind::Boolean :
                                 Expression::Kind::Integer;
    fn.result->integer = boolean ? 1 : 7;
    source.functions.push_back(std::move(fn));
    return source;
}

static void rejected(const Program& source, const std::string& message) {
    try {
        (void)oreslang::emit_verified_llvm_ir(source);
    } catch (const std::runtime_error& ex) {
        if (std::string(ex.what()).find(message) != std::string::npos)
            return;
        throw;
    }
    throw std::runtime_error("unverified source AST emitted native LLVM IR");
}

int main() {
    // Program is public AST input to the LLVM emitter. Even hand-built
    // Programs cannot spoof native Assert ABI symbols around the parser.
    rejected(program("oreslang_assert_equal_scalar_v0"),
             "reserved native runtime ABI symbol");
    rejected(program("oreslang_assert_trap_bool_v0", true),
             "reserved native runtime ABI symbol");
    rejected(program("oreslang_assert_future_v1"),
             "reserved native runtime ABI symbol");

    // No AST mutation may turn an impure Boolean function into a constant
    // i1 and thereby erase resource cleanup/effects from the guest program.
    {
        auto source = program("boolean_effect", true);
        source.functions.front().has_declared_effects = true;
        rejected(source, "unverified typed bool native result lowering");
    }
    {
        auto source = program("boolean_cleanup", true);
        source.functions.front().caught_throw_defers.emplace_back();
        rejected(source, "unverified typed bool native result lowering");
    }
    {
        auto source = program("boolean_mail", true);
        source.functions.front().actor_sends.emplace_back();
        rejected(source, "unverified typed bool native result lowering");
    }
    {
        auto source = program("boolean_noncanonical", true);
        source.functions.front().result->integer = 2;
        rejected(source, "unverified typed bool native result lowering");
    }
}
