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
static Program cooperative_source_frame() {
    Program p;
    CooperativeScalarLoop loop;
    loop.name = "step";
    loop.variable = "count";
    loop.initial = 0;
    p.cooperative_loops.push_back(std::move(loop));
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

    // Caller-constructed ordinary ASTs need the same fail-closed structural
    // admission as specialized frames. Null nodes must report a compiler
    // diagnostic instead of crashing the process before ownership checking.
    auto missing_local = main_with_use_after_take();
    missing_local.functions.front().locals.front().initializer.reset();
    expect_rejection([&] { (void)emit_verified_llvm_ir(missing_local); },
                     "malformed native ownership AST: local has no initializer");

    auto missing_binary = main_with_use_after_take();
    missing_binary.functions.front().result->right.reset();
    expect_rejection([&] { (void)emit_verified_llvm_ir(missing_binary); },
                     "malformed native ownership AST: missing binary operand");

    Program missing_call_argument;
    Function bad_call;
    bad_call.name = "main";
    auto call = make_expression(Expression::Kind::Call, "callee");
    call->arguments.push_back(nullptr);
    bad_call.result = std::move(call);
    missing_call_argument.functions.push_back(std::move(bad_call));
    expect_rejection([&] { (void)emit_verified_llvm_ir(missing_call_argument); },
                     "malformed native ownership AST: missing expression");

    Program missing_guard;
    Function bad_guard;
    bad_guard.name = "main";
    GuardedReturn guard;
    guard.lhs = make_expression(Expression::Kind::Integer, {}, 1);
    guard.result = make_expression(Expression::Kind::Integer, {}, 9);
    bad_guard.early_returns.push_back(std::move(guard));
    bad_guard.result = make_expression(Expression::Kind::Integer, {}, 2);
    missing_guard.functions.push_back(std::move(bad_guard));
    expect_rejection([&] { (void)emit_verified_llvm_ir(missing_guard); },
                     "malformed native ownership AST: guarded return has missing expression");

    Program missing_scoped;
    Function bad_scoped;
    bad_scoped.name = "main";
    Statement scoped;
    scoped.kind = Statement::Kind::ScopedLocal;
    bad_scoped.statements.push_back(std::move(scoped));
    bad_scoped.result = make_expression(Expression::Kind::Integer, {}, 0);
    missing_scoped.functions.push_back(std::move(bad_scoped));
    expect_rejection([&] { (void)emit_verified_llvm_ir(missing_scoped); },
                     "malformed native ownership AST: scoped local has no binding");

    Program missing_actor_payload;
    Function bad_sender;
    bad_sender.name = "main";
    ActorSend send;
    send.reference = "actor";
    bad_sender.actor_sends.push_back(std::move(send));
    bad_sender.result = make_expression(Expression::Kind::Integer, {}, 0);
    missing_actor_payload.functions.push_back(std::move(bad_sender));
    expect_rejection([&] { (void)emit_verified_llvm_ir(missing_actor_payload); },
                     "malformed native ownership AST: missing expression");

    Program hidden_leaf;
    Function bad_leaf;
    bad_leaf.name = "main";
    bad_leaf.result = make_expression(Expression::Kind::Integer, {}, 0);
    bad_leaf.result->left = make_expression(Expression::Kind::Take);
    hidden_leaf.functions.push_back(std::move(bad_leaf));
    expect_rejection([&] { (void)emit_verified_llvm_ir(hidden_leaf); },
                     "malformed native ownership AST: scalar leaf contains hidden operands");

    // A direct AST cannot define two same-name/same-user-arity overloads:
    // the semantic symbol table must not silently keep the first binding.
    Program duplicate_slots;
    Function first_overload;
    first_overload.name = "compute";
    first_overload.result = make_expression(Expression::Kind::Integer, {}, 1);
    duplicate_slots.functions.push_back(std::move(first_overload));
    Function second_overload;
    second_overload.name = "compute";
    second_overload.result = make_expression(Expression::Kind::Integer, {}, 2);
    duplicate_slots.functions.push_back(std::move(second_overload));
    expect_rejection([&] { (void)emit_verified_llvm_ir(duplicate_slots); },
                     "duplicate callable user arity");

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

    auto absent_initializer = bounded_scalar_frame();
    absent_initializer.functions.front().locals.front().initializer.reset();
    expect_rejection([&] { (void)emit_verified_llvm_ir(absent_initializer); },
                     "AST contains unlowered owners or effects");

    // Malformed AST leaf nodes can still contain hidden effectful children.
    // The specialized scalar emitter must reject them instead of ignoring
    // a move, borrow, call, or extra owner on a fake Integer/Variable leaf.
    auto initializer_with_hidden_move = bounded_scalar_frame();
    auto hidden_take = make_expression(Expression::Kind::Take);
    hidden_take->left = variable("other_owner");
    initializer_with_hidden_move.functions.front().locals.front()
        .initializer->arguments.push_back(std::move(hidden_take));
    expect_rejection([&] { (void)emit_verified_llvm_ir(initializer_with_hidden_move); },
                     "AST contains unlowered owners or effects");

    auto return_with_hidden_call = bounded_scalar_frame();
    return_with_hidden_call.functions.front().result->left =
        make_expression(Expression::Kind::Call, "hidden_effect");
    expect_rejection([&] { (void)emit_verified_llvm_ir(return_with_hidden_call); },
                     "AST contains unlowered owners or effects");

    auto initializer_with_generic_args = bounded_scalar_frame();
    initializer_with_generic_args.functions.front().locals.front()
        .initializer->type_arguments_present = true;
    expect_rejection([&] { (void)emit_verified_llvm_ir(initializer_with_generic_args); },
                     "AST contains unlowered owners or effects");

    auto hidden_constructor = bounded_scalar_frame();
    hidden_constructor.functions.front().locals.front()
        .constructor_arguments.push_back(
            make_expression(Expression::Kind::Call, "hidden_constructor"));
    expect_rejection([&] { (void)emit_verified_llvm_ir(hidden_constructor); },
                     "AST contains unlowered owners or effects");

    auto hidden_move_source = bounded_scalar_frame();
    hidden_move_source.functions.front().locals.front().move_source = "other_owner";
    expect_rejection([&] { (void)emit_verified_llvm_ir(hidden_move_source); },
                     "AST contains unlowered owners or effects");

    auto hidden_catch_binding = bounded_scalar_frame();
    hidden_catch_binding.functions.front().catch_binding = "exception";
    expect_rejection([&] { (void)emit_verified_llvm_ir(hidden_catch_binding); },
                     "AST contains unlowered owners or effects");

    auto invalid_function_name = bounded_scalar_frame();
    invalid_function_name.functions.front().name = "not-source-valid!";
    expect_rejection([&] { (void)emit_verified_llvm_ir(invalid_function_name); },
                     "AST contains unlowered owners or effects");

    auto invalid_counter_name = bounded_scalar_frame();
    invalid_counter_name.functions.front().scalar_while->counter = "actor.bad";
    invalid_counter_name.functions.front().locals.front().name = "actor.bad";
    invalid_counter_name.functions.front().result->name = "actor.bad";
    expect_rejection([&] { (void)emit_verified_llvm_ir(invalid_counter_name); },
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

    // Direct callers must not sneak unlowered users, extra frames, reserved
    // source identifiers, or incompatible targets past the parser's contract.
    auto cooperative = cooperative_source_frame();
    const auto cooperative_ir = emit_verified_llvm_ir(cooperative);
    check(cooperative_ir.find("oreslang_rt_step_resume") != std::string::npos,
          "valid cooperative frame could not emit LLVM IR");
    auto extra_cooperative = cooperative_source_frame();
    extra_cooperative.cooperative_loops.push_back(CooperativeScalarLoop{});
    expect_rejection([&] { (void)emit_verified_llvm_ir(extra_cooperative); },
                     "one standalone suspend function");
    auto mixed_cooperative = cooperative_source_frame();
    Function injected;
    injected.name = "hidden";
    injected.result = make_expression(Expression::Kind::Integer, {}, 1);
    mixed_cooperative.functions.push_back(std::move(injected));
    expect_rejection([&] { (void)emit_verified_llvm_ir(mixed_cooperative); },
                     "one standalone suspend function");
    auto module_cooperative = cooperative_source_frame();
    module_cooperative.module = "Hidden";
    expect_rejection([&] { (void)emit_verified_llvm_ir(module_cooperative); },
                     "one standalone suspend function");
    auto malformed_cooperative_name = cooperative_source_frame();
    malformed_cooperative_name.cooperative_loops.front().name = "step.bad";
    expect_rejection([&] { (void)emit_verified_llvm_ir(malformed_cooperative_name); },
                     "invalid source identifiers or initializer");
    auto malformed_cooperative_var = cooperative_source_frame();
    malformed_cooperative_var.cooperative_loops.front().variable = "bad-name";
    expect_rejection([&] { (void)emit_verified_llvm_ir(malformed_cooperative_var); },
                     "invalid source identifiers or initializer");
    auto reserved_cooperative = cooperative_source_frame();
    reserved_cooperative.cooperative_loops.front().name = "await";
    expect_rejection([&] { (void)emit_verified_llvm_ir(reserved_cooperative); },
                     "invalid source identifiers or initializer");
    auto reserved_capability_name = cooperative_source_frame();
    reserved_capability_name.cooperative_loops.front().variable = "ActorGroup";
    expect_rejection([&] { (void)emit_verified_llvm_ir(reserved_capability_name); },
                     "invalid source identifiers or initializer");
    auto negative_cooperative = cooperative_source_frame();
    negative_cooperative.cooperative_loops.front().initial = -1;
    expect_rejection([&] { (void)emit_verified_llvm_ir(negative_cooperative); },
                     "invalid source identifiers or initializer");
    expect_rejection([&] { (void)emit_verified_llvm_ir(cooperative, true, false); },
                     "one standalone suspend function");
    expect_rejection([&] { (void)emit_verified_llvm_ir(cooperative, false, true); },
                     "one standalone suspend function");

    std::cout << "NATIVE_LLVM_EMISSION_REVALIDATION_OK\n";
    return 0;
}
