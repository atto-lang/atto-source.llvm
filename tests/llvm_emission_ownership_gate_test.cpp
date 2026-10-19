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
static Program actor_bootstrap_program() {
    Program p;
    ActorDecl actor;
    actor.name = "Worker";
    LocalBinding state;
    state.name = "count";
    state.field_mutable = true;
    state.initializer = make_expression(Expression::Kind::Integer, {}, 0);
    actor.fields.push_back(std::move(state));
    actor.on_message.name = "on_message";
    Parameter message;
    message.name = "message";
    actor.on_message.parameters.push_back(std::move(message));
    actor.on_message.result = variable("message");
    p.actors.push_back(std::move(actor));

    Function main;
    main.name = "main";
    ActorSpawnBinding spawn;
    spawn.name = "worker";
    spawn.actor_type = "Worker";
    spawn.declared_type.kind = SourceType::Kind::ActorRef;
    spawn.declared_type.argument = "Worker";
    main.actor_spawns.push_back(std::move(spawn));
    main.actor_stops.push_back("worker");
    main.actor_actions.push_back({ActorAction::Kind::Stop, 0});
    main.result = make_expression(Expression::Kind::Integer, {}, 0);
    p.functions.push_back(std::move(main));
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

    // Source-impossible statement payloads must not be merely traversed:
    // later ownership/codegen switches consume only fields for the active kind.
    Program hidden_statement_field;
    Function bad_statement_fn;
    bad_statement_fn.name = "main";
    Statement bad_statement;
    bad_statement.kind = Statement::Kind::Call;
    bad_statement.expression = make_expression(Expression::Kind::Call, "callee");
    bad_statement.condition_lhs = make_expression(Expression::Kind::Take);
    bad_statement_fn.statements.push_back(std::move(bad_statement));
    bad_statement_fn.result = make_expression(Expression::Kind::Integer, {}, 0);
    hidden_statement_field.functions.push_back(std::move(bad_statement_fn));
    expect_rejection([&] { (void)emit_verified_llvm_ir(hidden_statement_field); },
                     "malformed native ownership AST: statement contains fields incompatible with its kind");

    // Ordinary locals have exactly one storage/initialization form. Hidden
    // constructor or move metadata on a scalar initializer would otherwise
    // be structurally walked but ignored by the normal ownership branch.
    Program hidden_local_metadata;
    Function bad_local_fn;
    bad_local_fn.name = "main";
    auto bad_local = scalar_local("x", 1);
    bad_local.constructor_arguments.push_back(
        make_expression(Expression::Kind::Take));
    bad_local_fn.locals.push_back(std::move(bad_local));
    bad_local_fn.result = variable("x");
    hidden_local_metadata.functions.push_back(std::move(bad_local_fn));
    expect_rejection([&] { (void)emit_verified_llvm_ir(hidden_local_metadata); },
                     "malformed native ownership AST: non-constructor local contains hidden constructor arguments");

    // actor_actions is not advisory: it is the exact source-order plan used
    // by codegen. Reject malformed indexes and omitted effects before LLVM.
    Program bad_actor_action_index;
    Function bad_action_fn;
    bad_action_fn.name = "main";
    ActorSend indexed_send;
    indexed_send.reference = "actor";
    indexed_send.payload = make_expression(Expression::Kind::Integer, {}, 1);
    bad_action_fn.actor_sends.push_back(std::move(indexed_send));
    bad_action_fn.actor_actions.push_back(
        ActorAction{ActorAction::Kind::Send, 1});
    bad_action_fn.result = make_expression(Expression::Kind::Integer, {}, 0);
    bad_actor_action_index.functions.push_back(std::move(bad_action_fn));
    expect_rejection([&] { (void)emit_verified_llvm_ir(bad_actor_action_index); },
                     "malformed native ownership AST: actor send action index is out of range");

    Program missing_actor_action;
    Function missing_action_fn;
    missing_action_fn.name = "main";
    ActorSend unplanned_send;
    unplanned_send.reference = "actor";
    unplanned_send.payload = make_expression(Expression::Kind::Integer, {}, 1);
    missing_action_fn.actor_sends.push_back(std::move(unplanned_send));
    missing_action_fn.result = make_expression(Expression::Kind::Integer, {}, 0);
    missing_actor_action.functions.push_back(std::move(missing_action_fn));
    expect_rejection([&] { (void)emit_verified_llvm_ir(missing_actor_action); },
                     "malformed native ownership AST: actor send is missing from source-order action plan");

    Program duplicate_actor_action;
    Function duplicate_action_fn;
    duplicate_action_fn.name = "main";
    ActorSend duplicate_send;
    duplicate_send.reference = "actor";
    duplicate_send.payload = make_expression(Expression::Kind::Integer, {}, 1);
    duplicate_action_fn.actor_sends.push_back(std::move(duplicate_send));
    duplicate_action_fn.actor_actions.push_back(
        ActorAction{ActorAction::Kind::Send, 0});
    duplicate_action_fn.actor_actions.push_back(
        ActorAction{ActorAction::Kind::Send, 0});
    duplicate_action_fn.result = make_expression(Expression::Kind::Integer, {}, 0);
    duplicate_actor_action.functions.push_back(std::move(duplicate_action_fn));
    expect_rejection([&] { (void)emit_verified_llvm_ir(duplicate_actor_action); },
                     "malformed native ownership AST: actor send action is duplicated");

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

    // Even a non-null AST can hide a moved/borrowed expression underneath a
    // source-impossible child slot. Those children must fail admission instead
    // of being ignored by subsequent ownership analysis or LLVM codegen.
    Program hidden_unary_operand;
    Function unary_fn;
    unary_fn.name = "main";
    auto unary = make_expression(Expression::Kind::Negate);
    unary->left = make_expression(Expression::Kind::Integer, {}, 1);
    unary->right = make_expression(Expression::Kind::Take);
    unary_fn.result = std::move(unary);
    hidden_unary_operand.functions.push_back(std::move(unary_fn));
    expect_rejection([&] { (void)emit_verified_llvm_ir(hidden_unary_operand); },
                     "malformed native ownership AST: unary or field expression contains hidden operands");

    Program hidden_call_receiver;
    Function call_fn;
    call_fn.name = "main";
    auto direct = make_expression(Expression::Kind::Call, "callee");
    direct->left = variable("unadmitted");
    call_fn.result = std::move(direct);
    hidden_call_receiver.functions.push_back(std::move(call_fn));
    expect_rejection([&] { (void)emit_verified_llvm_ir(hidden_call_receiver); },
                     "malformed native ownership AST: direct call contains hidden receiver or operand");

    Program hidden_binary_arguments;
    Function binary_fn;
    binary_fn.name = "main";
    auto binary = make_expression(Expression::Kind::Add);
    binary->left = make_expression(Expression::Kind::Integer, {}, 1);
    binary->right = make_expression(Expression::Kind::Integer, {}, 2);
    binary->arguments.push_back(make_expression(Expression::Kind::Take));
    binary_fn.result = std::move(binary);
    hidden_binary_arguments.functions.push_back(std::move(binary_fn));
    expect_rejection([&] { (void)emit_verified_llvm_ir(hidden_binary_arguments); },
                     "malformed native ownership AST: binary expression contains hidden arguments");

    Program hidden_leaf_metadata;
    Function metadata_fn;
    metadata_fn.name = "main";
    auto leaf = make_expression(Expression::Kind::Variable, "x");
    leaf->type_arguments_present = true;
    metadata_fn.locals.push_back(scalar_local("x", 42));
    metadata_fn.result = std::move(leaf);
    hidden_leaf_metadata.functions.push_back(std::move(metadata_fn));
    expect_rejection([&] { (void)emit_verified_llvm_ir(hidden_leaf_metadata); },
                     "malformed native ownership AST: non-call expression contains unlowered call metadata");

    // Direct-AST actor bootstraps must rebuild the parser's capability and
    // symbol tables before LLVM indexes generated actor maps.
    auto actor_program = actor_bootstrap_program();
    const auto actor_ir = emit_verified_llvm_ir(actor_program);
    check(actor_ir.find("oreslang_source_actor_spawn") != std::string::npos,
          "valid direct-AST actor bootstrap was rejected");

    auto unknown_actor_type = actor_bootstrap_program();
    unknown_actor_type.functions.front().actor_spawns.front().actor_type = "Ghost";
    unknown_actor_type.functions.front().actor_spawns.front().declared_type.argument = "Ghost";
    expect_rejection([&] { (void)emit_verified_llvm_ir(unknown_actor_type); },
                     "unknown actor type 'Ghost'");

    auto mismatched_actor_type = actor_bootstrap_program();
    mismatched_actor_type.functions.front().actor_spawns.front().declared_type.argument = "Other";
    expect_rejection([&] { (void)emit_verified_llvm_ir(mismatched_actor_type); },
                     "actor spawn declaration/type mismatch");

    auto unstopped_actor = actor_bootstrap_program();
    unstopped_actor.functions.front().actor_stops.clear();
    unstopped_actor.functions.front().actor_actions.clear();
    expect_rejection([&] { (void)emit_verified_llvm_ir(unstopped_actor); },
                     "actor reference must be stopped before return");

    auto send_after_stop = actor_bootstrap_program();
    ActorSend late_send;
    late_send.reference = "worker";
    late_send.payload = make_expression(Expression::Kind::Integer, {}, 1);
    send_after_stop.functions.front().actor_sends.push_back(std::move(late_send));
    send_after_stop.functions.front().actor_actions.push_back(
        {ActorAction::Kind::Send, 0});
    expect_rejection([&] { (void)emit_verified_llvm_ir(send_after_stop); },
                     "actor message sent after stop 'worker'");

    auto missing_actor_parameter = actor_bootstrap_program();
    missing_actor_parameter.actors.front().on_message.parameters.clear();
    expect_rejection([&] { (void)emit_verified_llvm_ir(missing_actor_parameter); },
                     "malformed direct-AST actor handler");

    auto hidden_actor_storage = actor_bootstrap_program();
    hidden_actor_storage.actors.front().fields.front().initializer.reset();
    hidden_actor_storage.actors.front().fields.front().int_array = true;
    expect_rejection([&] { (void)emit_verified_llvm_ir(hidden_actor_storage); },
                     "native actor state requires scalar initialized fields");

    Program duplicate_class_fields;
    ClassDecl duplicate_class;
    duplicate_class.name = "Pair";
    duplicate_class.fields.push_back(ClassField{"x"});
    duplicate_class.fields.push_back(ClassField{"x"});
    duplicate_class_fields.classes.push_back(std::move(duplicate_class));
    Function class_main;
    class_main.name = "main";
    class_main.result = make_expression(Expression::Kind::Integer, {}, 0);
    duplicate_class_fields.functions.push_back(std::move(class_main));
    expect_rejection([&] { (void)emit_verified_llvm_ir(duplicate_class_fields); },
                     "duplicate class field 'x'");

    Program duplicate_locals;
    Function duplicate_local_main;
    duplicate_local_main.name = "main";
    duplicate_local_main.locals.push_back(scalar_local("x", 1));
    duplicate_local_main.locals.push_back(scalar_local("x", 2));
    duplicate_local_main.result = variable("x");
    duplicate_locals.functions.push_back(std::move(duplicate_local_main));
    expect_rejection([&] { (void)emit_verified_llvm_ir(duplicate_locals); },
                     "duplicate owned local 'x'");

    // Unselected statement payload slots must never be silently ignored by
    // the LLVM lowering path. Ownership traversal alone is not sufficient.
    Program hidden_statement_condition;
    Function hidden_condition_fn;
    hidden_condition_fn.name = "main";
    Statement hidden_condition;
    hidden_condition.kind = Statement::Kind::Call;
    hidden_condition.expression = make_expression(Expression::Kind::Call, "callee");
    hidden_condition.condition_lhs = make_expression(Expression::Kind::Take);
    hidden_condition_fn.statements.push_back(std::move(hidden_condition));
    hidden_condition_fn.result = make_expression(Expression::Kind::Integer, {}, 0);
    hidden_statement_condition.functions.push_back(std::move(hidden_condition_fn));
    expect_rejection([&] { (void)emit_verified_llvm_ir(hidden_statement_condition); },
                     "statement contains hidden conditional effects");

    Program hidden_statement_binding;
    Function hidden_binding_fn;
    hidden_binding_fn.name = "main";
    Statement hidden_binding;
    hidden_binding.kind = Statement::Kind::Call;
    hidden_binding.expression = make_expression(Expression::Kind::Call, "callee");
    hidden_binding.branch_local = std::make_unique<LocalBinding>(
        scalar_local("secret", 1));
    hidden_binding_fn.statements.push_back(std::move(hidden_binding));
    hidden_binding_fn.result = make_expression(Expression::Kind::Integer, {}, 0);
    hidden_statement_binding.functions.push_back(std::move(hidden_binding_fn));
    expect_rejection([&] { (void)emit_verified_llvm_ir(hidden_statement_binding); },
                     "statement contains hidden scoped binding");

    Program hidden_statement_array_commit;
    Function hidden_array_fn;
    hidden_array_fn.name = "main";
    Statement hidden_array;
    hidden_array.kind = Statement::Kind::Call;
    hidden_array.expression = make_expression(Expression::Kind::Call, "callee");
    hidden_array.array_commit_length = make_expression(Expression::Kind::Integer, {}, 5);
    hidden_array_fn.statements.push_back(std::move(hidden_array));
    hidden_array_fn.result = make_expression(Expression::Kind::Integer, {}, 0);
    hidden_statement_array_commit.functions.push_back(std::move(hidden_array_fn));
    expect_rejection([&] { (void)emit_verified_llvm_ir(hidden_statement_array_commit); },
                     "statement contains hidden array commit");

    // The actor action vector is the emitter's sole effect schedule. Reject
    // omitted effects, duplicates, invalid indices, and unknown variants.
    Program orphan_send;
    Function orphan_fn;
    orphan_fn.name = "main";
    ActorSend orphan;
    orphan.reference = "worker";
    orphan.payload = make_expression(Expression::Kind::Integer, {}, 7);
    orphan_fn.actor_sends.push_back(std::move(orphan));
    orphan_fn.result = make_expression(Expression::Kind::Integer, {}, 0);
    orphan_send.functions.push_back(std::move(orphan_fn));
    expect_rejection([&] { (void)emit_verified_llvm_ir(orphan_send); },
                     "actor effect omitted from ordered actions");

    Program orphan_stop;
    Function stop_fn;
    stop_fn.name = "main";
    stop_fn.actor_stops.push_back("worker");
    stop_fn.result = make_expression(Expression::Kind::Integer, {}, 0);
    orphan_stop.functions.push_back(std::move(stop_fn));
    expect_rejection([&] { (void)emit_verified_llvm_ir(orphan_stop); },
                     "actor effect omitted from ordered actions");

    Program duplicate_send_action;
    Function duplicate_action_fn;
    duplicate_action_fn.name = "main";
    ActorSend scheduled;
    scheduled.reference = "worker";
    scheduled.payload = make_expression(Expression::Kind::Integer, {}, 7);
    duplicate_action_fn.actor_sends.push_back(std::move(scheduled));
    duplicate_action_fn.actor_actions.push_back({ActorAction::Kind::Send, 0});
    duplicate_action_fn.actor_actions.push_back({ActorAction::Kind::Send, 0});
    duplicate_action_fn.result = make_expression(Expression::Kind::Integer, {}, 0);
    duplicate_send_action.functions.push_back(std::move(duplicate_action_fn));
    expect_rejection([&] { (void)emit_verified_llvm_ir(duplicate_send_action); },
                     "invalid or repeated actor send action");

    Program bad_action_index;
    Function indexed_fn;
    indexed_fn.name = "main";
    indexed_fn.actor_actions.push_back({ActorAction::Kind::Send, 0});
    indexed_fn.result = make_expression(Expression::Kind::Integer, {}, 0);
    bad_action_index.functions.push_back(std::move(indexed_fn));
    expect_rejection([&] { (void)emit_verified_llvm_ir(bad_action_index); },
                     "invalid or repeated actor send action");

    Program unknown_action_kind;
    Function unknown_action_fn;
    unknown_action_fn.name = "main";
    unknown_action_fn.actor_actions.push_back({
        static_cast<ActorAction::Kind>(255), 0});
    unknown_action_fn.result = make_expression(Expression::Kind::Integer, {}, 0);
    unknown_action_kind.functions.push_back(std::move(unknown_action_fn));
    expect_rejection([&] { (void)emit_verified_llvm_ir(unknown_action_kind); },
                     "unknown actor action kind");

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
