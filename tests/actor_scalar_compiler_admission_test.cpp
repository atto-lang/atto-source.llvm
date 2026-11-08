#include "oreslang/backend/ActorScalarFrameLowering.hpp"
#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

using namespace oreslang;

namespace {
std::unique_ptr<Expression> node(Expression::Kind kind) {
    auto expression = std::make_unique<Expression>();
    expression->kind = kind;
    return expression;
}
std::unique_ptr<Expression> variable(const std::string& name) {
    auto expression = node(Expression::Kind::Variable);
    expression->name = name;
    return expression;
}
std::unique_ptr<Expression> integer(std::int64_t value) {
    auto expression = node(Expression::Kind::Integer);
    expression->integer = value;
    return expression;
}
std::unique_ptr<Expression> right_nested(unsigned count) {
    auto result = variable("n");
    for (unsigned i = 0; i < count; ++i) {
        auto sum = node(Expression::Kind::Add);
        sum->left = integer(1);
        sum->right = std::move(result);
        result = std::move(sum);
    }
    return result;
}
std::unique_ptr<Expression> left_nested(unsigned count) {
    auto result = variable("n");
    for (unsigned i = 0; i < count; ++i) {
        auto sum = node(Expression::Kind::Add);
        sum->left = std::move(result);
        sum->right = integer(1);
        result = std::move(sum);
    }
    return result;
}
std::unique_ptr<Expression> negated(unsigned count) {
    auto result = variable("n");
    for (unsigned i = 0; i < count; ++i) {
        auto neg = node(Expression::Kind::Negate);
        neg->left = std::move(result);
        result = std::move(neg);
    }
    return result;
}
Parameter i64_param() {
    Parameter parameter;
    parameter.name = "n";
    parameter.scalar_type = ScalarType::I64;
    parameter.mode = Parameter::Mode::Value;
    return parameter;
}
Program with_fn(std::unique_ptr<Expression> result) {
    Program program;
    Function function;
    function.name = "nested";
    function.parameters.push_back(i64_param());
    function.result = std::move(result);
    program.functions.push_back(std::move(function));
    return program;
}
ActorDecl handler() {
    ActorDecl actor;
    actor.name = "Worker";
    actor.on_message.name = "on_message";
    actor.on_message.parameters.push_back(i64_param());
    actor.on_message.return_type = ScalarType::I64;
    auto call = node(Expression::Kind::Call);
    call->name = "nested";
    call->arguments.push_back(variable("n"));
    actor.on_message.result = std::move(call);
    return actor;
}
template <typename F>
void denied(F&& run, const std::string& expected) {
    bool caught = false;
    try {
        run();
    } catch (const std::runtime_error& error) {
        caught = true;
        assert(std::string(error.what()).find(expected) != std::string::npos);
    }
    assert(caught && "unsafe graph was admitted instead of rejected");
}
} // namespace

int main() {
    // 15 right-nested additions retain exactly sixteen live i64 cells.
    {
        auto program = with_fn(right_nested(15));
        auto actor = handler();
        const auto compiled = compile_actor_scalar_graph(program, actor);
        assert(compiled.has_value());
        assert(compiled->entries.size() == 1);
        assert(compiled->code.size() == 32);
        assert(compiled->code[0].opcode == ORES_SCALAR_PUSH_I64);
        assert(compiled->code.back().opcode == ORES_SCALAR_RETURN);
    }
    // 16 additions need seventeen cells. Do not create LLVM IR that
    // will fault an otherwise compiler-accepted actor at runtime.
    {
        auto program = with_fn(right_nested(16));
        auto actor = handler();
        denied([&] { (void)compile_actor_scalar_graph(program, actor); },
            "typed scalar operand stack exceeds 16 slots");
    }
    // The left-deep equivalent has only two simultaneous operands;
    // reject by *live values*, not raw arithmetic operator count.
    {
        auto program = with_fn(left_nested(32));
        auto actor = handler();
        const auto compiled = compile_actor_scalar_graph(program, actor);
        assert(compiled && compiled->code.size() == 66);
    }
    // Each unary negate leaves a leading zero on the operand stack.
    {
        auto program = with_fn(negated(15));
        auto actor = handler();
        assert(compile_actor_scalar_graph(program, actor).has_value());
    }
    {
        auto program = with_fn(negated(16));
        auto actor = handler();
        denied([&] { (void)compile_actor_scalar_graph(program, actor); },
            "typed scalar operand stack exceeds 16 slots");
    }
    // Handler signatures are checked *before* parameter[0] access.
    {
        auto program = with_fn(variable("n"));
        auto actor = handler();
        actor.on_message.parameters.clear();
        denied([&] { (void)compile_actor_scalar_graph(program, actor); },
            "actor callback may only directly return pure unary fnc");
    }
    // No silently ignored handler effects or side-effecting prelude.
    {
        auto program = with_fn(variable("n"));
        auto actor = handler();
        actor.on_message.has_declared_effects = true;
        denied([&] { (void)compile_actor_scalar_graph(program, actor); },
            "actor callback may only directly return pure unary fnc");
    }
    {
        auto program = with_fn(variable("n"));
        auto actor = handler();
        actor.on_message.early_returns.emplace_back();
        denied([&] { (void)compile_actor_scalar_graph(program, actor); },
            "actor callback may only directly return pure unary fnc");
    }
    {
        auto program = with_fn(variable("n"));
        auto actor = handler();
        actor.on_message.result->argument_names.push_back("n");
        denied([&] { (void)compile_actor_scalar_graph(program, actor); },
            "actor callback may only directly return pure unary fnc");
    }
    // Preserve the language rule: recursive routines are not fnc.
    {
        auto program = with_fn(variable("n"));
        program.functions[0].direct_only_routine = true;
        auto actor = handler();
        denied([&] { (void)compile_actor_scalar_graph(program, actor); },
            "function graph is not a closed pure unary integer fnc subset");
    }
    // Compiler graph lowering is single-source, no imports/FFI fallback.
    {
        auto program = with_fn(variable("n"));
        program.imports.emplace_back();
        auto actor = handler();
        denied([&] { (void)compile_actor_scalar_graph(program, actor); },
            "source module imports are not admitted");
    }
    return EXIT_SUCCESS;
}
