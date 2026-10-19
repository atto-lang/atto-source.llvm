#include "oreslang/backend/LlvmCodegen.hpp"
#include "oreslang/frontend/Parser.hpp"

#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

using namespace oreslang;

static void check(bool ok, const char* reason) {
    if (!ok) throw std::runtime_error(reason);
}
static std::unique_ptr<Expression> make_expression(
        Expression::Kind kind, std::string name = {}, std::int64_t value = 0) {
    auto e = std::make_unique<Expression>();
    e->kind = kind;
    e->name = std::move(name);
    e->integer = value;
    return e;
}
static std::unique_ptr<Expression> variable(std::string name) {
    return make_expression(Expression::Kind::Variable, std::move(name));
}
static LocalBinding scalar_local(std::string name, std::int64_t initial) {
    LocalBinding binding;
    binding.name = std::move(name);
    binding.owner_mutable = true;
    binding.initializer = make_expression(Expression::Kind::Integer, {}, initial);
    return binding;
}
template<class Action>
static void expect_rejection(Action&& action, const std::string& diagnostic) {
    bool matched = false;
    try { action(); }
    catch (const std::runtime_error& e) {
        matched = std::string(e.what()).find(diagnostic) != std::string::npos;
    }
    check(matched, "unsafe or malformed AST passed the LLVM emission gate");
}
static Program main_with_use_after_take() {
    Program p;
    Function f;
    f.name = "main";
    f.locals.push_back(scalar_local("x", 42));
    auto taken = make_expression(Expression::Kind::Take);
    taken->left = variable("x");
    auto sum = make_expression(Expression::Kind::Add);
    sum->left = std::move(taken);
    sum->right = variable("x"); // read-after-move: never emit LLVM IR
    f.result = std::move(sum);
    p.functions.push_back(std::move(f));
    return p;
}
static Program bounded_scalar_frame() {
    Program p;
    Function f;
    f.name = "count_to_ten";
    f.locals.push_back(scalar_local("count", 0));
    f.scalar_while = std::make_unique<ScalarWhileLoop>();
    f.scalar_while->counter = "count";
    f.scalar_while->initial = 0;
    f.scalar_while->upper_bound = 10;
    f.result = variable("count");
    p.functions.push_back(std::move(f));
    return p;
}
int main() {
    // A direct C++ caller must not bypass parse_program's ownership check.
    auto invalid = main_with_use_after_take();
    expect_rejection([&] { (void)emit_verified_llvm_ir(invalid); },
                     "use of explicitly moved int");

    // Keep a positive ordinary source program and specialized scalar frame.
    auto simple = parse_program(
        "pub fnc main(): int { val x: int = 42; return x; }");
    const auto simple_ir = emit_verified_llvm_ir(simple);
    check(simple_ir.find("define") != std::string::npos,
          "valid source cannot emit LLVM IR");

    auto scalar = bounded_scalar_frame();
    const auto frame_ir = emit_verified_llvm_ir(scalar);
    check(frame_ir.find("oreslang_rt_scalar_frame_step") != std::string::npos,
          "safe stand-alone scalar frame was rejected");

    // Source-legal frames have exactly one owned scalar local. These direct
    // mutations mimic post-parse IR passes: they must not silently omit
    // owned locals, effects, or entire other functions.
    auto extra_owner = bounded_scalar_frame();
    extra_owner.functions.front().locals.push_back(scalar_local("hidden", 99));
    expect_rejection([&] { (void)emit_verified_llvm_ir(extra_owner); },
                     "AST contains unlowered owners or effects");

    auto inconsistent_initializer = bounded_scalar_frame();
    inconsistent_initializer.functions.front().locals.front().initializer->integer = 19;
    expect_rejection([&] { (void)emit_verified_llvm_ir(inconsistent_initializer); },
                     "AST contains unlowered owners or effects");

    auto mixed = bounded_scalar_frame();
    Function other;
    other.name = "extra";
    other.result = make_expression(Expression::Kind::Integer, {}, 2);
    mixed.functions.push_back(std::move(other));
    expect_rejection([&] { (void)emit_verified_llvm_ir(mixed); },
                     "one standalone native function");

    auto module_wrapped = bounded_scalar_frame();
    module_wrapped.module = "Hidden";
    expect_rejection([&] { (void)emit_verified_llvm_ir(module_wrapped); },
                     "one standalone native function");

    std::cout << "NATIVE_LLVM_EMISSION_REVALIDATION_OK\n";
    return 0;
}
