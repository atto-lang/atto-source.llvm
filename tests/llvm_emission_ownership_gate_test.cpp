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
    std::string actual = "<no runtime_error>";
    try { action(); }
    catch (const std::runtime_error& e) {
        actual = e.what();
        matched = actual.find(diagnostic) != std::string::npos;
    }
    if (!matched)
        throw std::runtime_error(
            "unsafe or malformed AST passed the LLVM emission gate; expected diagnostic '" +
            diagnostic + "', got '" + actual + "'");
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
    actor.on_message.exported = true;
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

    Program missing_final_result;
    Function no_result;
    no_result.name = "main";
    missing_final_result.functions.push_back(std::move(no_result));
    expect_rejection([&] { (void)emit_verified_llvm_ir(missing_final_result); },
                     "malformed native ownership AST: non-void function has no final result");

    Program void_hidden_result;
    Function void_value;
    void_value.name = "main";
    void_value.returns_void = true;
    void_value.result = make_expression(Expression::Kind::Integer, {}, 7);
    void_hidden_result.functions.push_back(std::move(void_value));
    expect_rejection([&] { (void)emit_verified_llvm_ir(void_hidden_result); },
                     "malformed native ownership AST: void function contains return value");

    Program void_caught_throw;
    Function void_throw;
    void_throw.name = "main";
    void_throw.returns_void = true;
    void_throw.caught_throw_value =
        make_expression(Expression::Kind::Integer, {}, 1);
    void_throw.catch_binding = "error";
    void_caught_throw.functions.push_back(std::move(void_throw));
    expect_rejection([&] { (void)emit_verified_llvm_ir(void_caught_throw); },
                     "malformed native ownership AST: caught throw requires non-void function");

    Program caught_throw_with_call_statement;
    Function bad_throw_shape;
    bad_throw_shape.name = "main";
    bad_throw_shape.caught_throw_value =
        make_expression(Expression::Kind::Integer, {}, 1);
    bad_throw_shape.catch_binding = "error";
    Statement hidden_call;
    hidden_call.kind = Statement::Kind::Call;
    hidden_call.expression =
        make_expression(Expression::Kind::Call, "helper");
    bad_throw_shape.statements.push_back(std::move(hidden_call));
    bad_throw_shape.result =
        make_expression(Expression::Kind::Integer, {}, 0);
    caught_throw_with_call_statement.functions.push_back(
        std::move(bad_throw_shape));
    expect_rejection([&] {
        (void)emit_verified_llvm_ir(caught_throw_with_call_statement);
    }, "malformed native ownership AST: caught throw finally contains non-field cleanup");

    Program oversized_constructor_signature;
    ClassDecl wide_ctor;
    wide_ctor.name = "Wide";
    wide_ctor.explicit_constructor = true;
    for (int i = 0; i < 17; ++i)
        wide_ctor.constructor_parameters.push_back("p" + std::to_string(i));
    ClassField wide_field;
    wide_field.name = "value";
    wide_field.initializer_expression =
        make_expression(Expression::Kind::Integer, {}, 0);
    wide_ctor.fields.push_back(std::move(wide_field));
    oversized_constructor_signature.classes.push_back(std::move(wide_ctor));
    Function wide_main;
    wide_main.name = "main";
    wide_main.result = make_expression(Expression::Kind::Integer, {}, 0);
    oversized_constructor_signature.functions.push_back(std::move(wide_main));
    expect_rejection([&] {
        (void)emit_verified_llvm_ir(oversized_constructor_signature);
    }, "malformed native ownership AST: class constructor parameter count exceeds parser bound");

    Program orphan_catch_metadata;
    Function orphan_catch;
    orphan_catch.name = "main";
    orphan_catch.catch_binding = "error";
    orphan_catch.result = make_expression(Expression::Kind::Integer, {}, 0);
    orphan_catch_metadata.functions.push_back(std::move(orphan_catch));
    expect_rejection([&] { (void)emit_verified_llvm_ir(orphan_catch_metadata); },
                     "malformed native ownership AST: catch metadata exists without caught throw");

    Program hidden_array_field_initializer;
    ClassDecl array_class;
    array_class.name = "Arrays";
    ClassField array_field;
    array_field.name = "items";
    array_field.int_array = true;
    array_field.mutable_referent = true;
    array_field.initializer_expression =
        make_expression(Expression::Kind::Integer, {}, 99);
    array_class.fields.push_back(std::move(array_field));
    hidden_array_field_initializer.classes.push_back(std::move(array_class));
    Function array_main;
    array_main.name = "main";
    array_main.result = make_expression(Expression::Kind::Integer, {}, 0);
    hidden_array_field_initializer.functions.push_back(std::move(array_main));
    expect_rejection([&] { (void)emit_verified_llvm_ir(hidden_array_field_initializer); },
                     "malformed native ownership AST: Array<int> class field contains hidden scalar initializer");

    Program missing_scalar_field_initializer;
    ClassDecl scalar_class;
    scalar_class.name = "Box";
    ClassField scalar_field;
    scalar_field.name = "value";
    scalar_class.fields.push_back(std::move(scalar_field));
    missing_scalar_field_initializer.classes.push_back(std::move(scalar_class));
    Function scalar_main;
    scalar_main.name = "main";
    scalar_main.result = make_expression(Expression::Kind::Integer, {}, 0);
    missing_scalar_field_initializer.functions.push_back(std::move(scalar_main));
    expect_rejection([&] { (void)emit_verified_llvm_ir(missing_scalar_field_initializer); },
                     "malformed native ownership AST: scalar class field is missing initializer");

    Program inconsistent_scalar_field_literal;
    ClassDecl literal_class;
    literal_class.name = "LiteralBox";
    ClassField literal_field;
    literal_field.name = "value";
    literal_field.initial = 8;
    literal_field.initializer_expression =
        make_expression(Expression::Kind::Integer, {}, 7);
    literal_class.fields.push_back(std::move(literal_field));
    inconsistent_scalar_field_literal.classes.push_back(std::move(literal_class));
    Function literal_main;
    literal_main.name = "main";
    literal_main.result = make_expression(Expression::Kind::Integer, {}, 0);
    inconsistent_scalar_field_literal.functions.push_back(std::move(literal_main));
    expect_rejection([&] { (void)emit_verified_llvm_ir(inconsistent_scalar_field_literal); },
                     "malformed native ownership AST: scalar class field literal cache disagrees with initializer");

    Program ordinary_actor_field_write;
    Function forged_actor_write;
    forged_actor_write.name = "main";
    ActorFieldWrite ignored_write;
    ignored_write.field = "count";
    ignored_write.expression =
        make_expression(Expression::Kind::Integer, {}, 1);
    forged_actor_write.actor_field_writes.push_back(std::move(ignored_write));
    forged_actor_write.result =
        make_expression(Expression::Kind::Integer, {}, 0);
    ordinary_actor_field_write.functions.push_back(std::move(forged_actor_write));
    expect_rejection([&] { (void)emit_verified_llvm_ir(ordinary_actor_field_write); },
                     "malformed native ownership AST: ordinary function contains actor field writes");

    Program invalid_parameter_mode;
    Function bad_mode;
    bad_mode.name = "identity";
    Parameter bad_parameter;
    bad_parameter.name = "value";
    bad_parameter.mode = static_cast<Parameter::Mode>(255);
    bad_mode.parameters.push_back(std::move(bad_parameter));
    bad_mode.result = variable("value");
    invalid_parameter_mode.functions.push_back(std::move(bad_mode));
    expect_rejection([&] { (void)emit_verified_llvm_ir(invalid_parameter_mode); },
                     "ownership violation: unknown parameter ownership mode");

    Program reserved_runtime_symbol;
    Function forged_abort;
    forged_abort.name = "abort";
    forged_abort.result =
        make_expression(Expression::Kind::Integer, {}, 0);
    reserved_runtime_symbol.functions.push_back(std::move(forged_abort));
    expect_rejection([&] { (void)emit_verified_llvm_ir(reserved_runtime_symbol); },
                     "ownership violation: reserved native runtime ABI symbol 'abort'");

    Program invalid_function_identifier;
    Function malformed_name;
    malformed_name.name = "bad-name";
    malformed_name.result =
        make_expression(Expression::Kind::Integer, {}, 0);
    invalid_function_identifier.functions.push_back(std::move(malformed_name));
    expect_rejection([&] { (void)emit_verified_llvm_ir(invalid_function_identifier); },
                     "ownership violation: invalid native function identifier");

    auto unexported_actor = actor_bootstrap_program();
    unexported_actor.actors.front().on_message.exported = false;
    expect_rejection([&] { (void)emit_verified_llvm_ir(unexported_actor); },
                     "ownership violation: native actor handler must be exported");

    Program unresolved_direct_call;
    Function unresolved_call_main;
    unresolved_call_main.name = "main";
    unresolved_call_main.result =
        make_expression(Expression::Kind::Call, "missing");
    unresolved_direct_call.functions.push_back(std::move(unresolved_call_main));
    expect_rejection([&] { (void)emit_verified_llvm_ir(unresolved_direct_call); },
                     "internal ownership error: unresolved function");

    Program unknown_constructor_reference;
    ClassDecl ctor_class;
    ctor_class.name = "Box";
    ctor_class.explicit_constructor = true;
    ctor_class.constructor_parameters.push_back("seed");
    ClassField ctor_field;
    ctor_field.name = "value";
    ctor_field.initializer_expression =
        make_expression(Expression::Kind::Variable, "ghost");
    ctor_class.fields.push_back(std::move(ctor_field));
    unknown_constructor_reference.classes.push_back(std::move(ctor_class));
    Function ctor_main;
    ctor_main.name = "main";
    ctor_main.result = make_expression(Expression::Kind::Integer, {}, 0);
    unknown_constructor_reference.functions.push_back(std::move(ctor_main));
    expect_rejection([&] { (void)emit_verified_llvm_ir(unknown_constructor_reference); },
                     "ownership violation: unknown class constructor parameter 'ghost'");

    Program impure_constructor_initializer;
    ClassDecl impure_class;
    impure_class.name = "Impure";
    ClassField impure_field;
    impure_field.name = "value";
    impure_field.initializer_expression =
        make_expression(Expression::Kind::Call, "callee");
    impure_class.fields.push_back(std::move(impure_field));
    impure_constructor_initializer.classes.push_back(std::move(impure_class));
    Function impure_main;
    impure_main.name = "main";
    impure_main.result = make_expression(Expression::Kind::Integer, {}, 0);
    impure_constructor_initializer.functions.push_back(std::move(impure_main));
    expect_rejection([&] { (void)emit_verified_llvm_ir(impure_constructor_initializer); },
                     "ownership violation: class field initializer requires pure scalar constructor expression");

    Program invalid_module_name;
    invalid_module_name.module = "bad-module";
    Function module_main;
    module_main.name = "main";
    module_main.result = make_expression(Expression::Kind::Integer, {}, 0);
    invalid_module_name.functions.push_back(std::move(module_main));
    expect_rejection([&] { (void)emit_verified_llvm_ir(invalid_module_name); },
                     "ownership violation: invalid native module identifier");

    Program hidden_generic_arguments;
    Function generic_identity;
    generic_identity.name = "identity";
    GenericParameter type_slot;
    type_slot.name = "T";
    generic_identity.generic_parameters.push_back(std::move(type_slot));
    Parameter generic_value;
    generic_value.name = "value";
    generic_value.generic_type = "T";
    generic_identity.parameters.push_back(std::move(generic_value));
    generic_identity.result = variable("value");
    hidden_generic_arguments.functions.push_back(std::move(generic_identity));
    Function hidden_generic_main;
    hidden_generic_main.name = "main";
    auto hidden_generic_call =
        make_expression(Expression::Kind::Call, "identity");
    hidden_generic_call->arguments.push_back(
        make_expression(Expression::Kind::Integer, {}, 1));
    GenericTypeArgument hidden_type;
    hidden_type.type = "int";
    hidden_generic_call->type_arguments.push_back(std::move(hidden_type));
    hidden_generic_main.result = std::move(hidden_generic_call);
    hidden_generic_arguments.functions.push_back(std::move(hidden_generic_main));
    expect_rejection([&] { (void)emit_verified_llvm_ir(hidden_generic_arguments); },
                     "native call contains hidden generic type arguments");

    Program nongeneric_specialization;
    Function plain_identity;
    plain_identity.name = "plain";
    Parameter plain_value;
    plain_value.name = "value";
    plain_identity.parameters.push_back(std::move(plain_value));
    plain_identity.result = variable("value");
    nongeneric_specialization.functions.push_back(std::move(plain_identity));
    Function nongeneric_main;
    nongeneric_main.name = "main";
    auto specialized_plain =
        make_expression(Expression::Kind::Call, "plain");
    specialized_plain->arguments.push_back(
        make_expression(Expression::Kind::Integer, {}, 1));
    specialized_plain->type_arguments_present = true;
    nongeneric_main.result = std::move(specialized_plain);
    nongeneric_specialization.functions.push_back(std::move(nongeneric_main));
    expect_rejection([&] { (void)emit_verified_llvm_ir(nongeneric_specialization); },
                     "call-site type arguments require generic function");

    Program self_referential_generic_default;
    Function bad_default;
    bad_default.name = "bad_default";
    GenericParameter self_default;
    self_default.name = "T";
    self_default.default_type = "T";
    bad_default.generic_parameters.push_back(std::move(self_default));
    bad_default.result = make_expression(Expression::Kind::Integer, {}, 0);
    self_referential_generic_default.functions.push_back(std::move(bad_default));
    expect_rejection([&] { (void)emit_verified_llvm_ir(self_referential_generic_default); },
                     "ownership violation: forward or unknown generic default 'T'");

    Program unknown_formal_generic;
    Function bad_formal;
    bad_formal.name = "bad_formal";
    GenericParameter declared_generic;
    declared_generic.name = "T";
    bad_formal.generic_parameters.push_back(std::move(declared_generic));
    Parameter unknown_generic_parameter;
    unknown_generic_parameter.name = "value";
    unknown_generic_parameter.generic_type = "U";
    bad_formal.parameters.push_back(std::move(unknown_generic_parameter));
    bad_formal.result = variable("value");
    unknown_formal_generic.functions.push_back(std::move(bad_formal));
    expect_rejection([&] { (void)emit_verified_llvm_ir(unknown_formal_generic); },
                     "ownership violation: parameter references unknown generic 'U'");

    Program malformed_inference_hole;
    Function hole_identity;
    hole_identity.name = "hole_identity";
    GenericParameter hole_slot;
    hole_slot.name = "T";
    hole_identity.generic_parameters.push_back(std::move(hole_slot));
    Parameter hole_value;
    hole_value.name = "value";
    hole_value.generic_type = "T";
    hole_identity.parameters.push_back(std::move(hole_value));
    hole_identity.result = variable("value");
    malformed_inference_hole.functions.push_back(std::move(hole_identity));
    Function hole_main;
    hole_main.name = "main";
    auto bad_hole_call =
        make_expression(Expression::Kind::Call, "hole_identity");
    bad_hole_call->arguments.push_back(
        make_expression(Expression::Kind::Integer, {}, 1));
    bad_hole_call->type_arguments_present = true;
    GenericTypeArgument bad_hole;
    bad_hole.infer = true;
    bad_hole.type = "int";
    bad_hole_call->type_arguments.push_back(std::move(bad_hole));
    hole_main.result = std::move(bad_hole_call);
    malformed_inference_hole.functions.push_back(std::move(hole_main));
    expect_rejection([&] { (void)emit_verified_llvm_ir(malformed_inference_hole); },
                     "generic inference hole contains explicit type");

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

    // The structural recursion budget is exact: depth 255 is admitted
    // (root depth is zero), while the next child at depth 256 fails closed.
    const auto nested_negations = [](unsigned wrappers) {
        Program p;
        Function f;
        f.name = "main";
        auto value = make_expression(Expression::Kind::Integer, {}, 1);
        for (unsigned i = 0; i < wrappers; ++i) {
            auto negate = make_expression(Expression::Kind::Negate);
            negate->left = std::move(value);
            value = std::move(negate);
        }
        f.result = std::move(value);
        p.functions.push_back(std::move(f));
        return p;
    };
    auto max_expression_depth = nested_negations(255);
    const auto max_expression_ir = emit_verified_llvm_ir(max_expression_depth);
    check(max_expression_ir.find("define") != std::string::npos,
          "maximum admitted expression nesting was rejected");
    auto excessive_expression_depth = nested_negations(256);
    expect_rejection([&] { (void)emit_verified_llvm_ir(excessive_expression_depth); },
                     "malformed native ownership AST: expression nesting exceeds 256");

    // Statement recursion has its own lower bound. Preflight must admit
    // the deepest representable tree below the cap and reject the next
    // statement before ownership analysis can recursively consume it.
    const auto nested_conditionals = [](unsigned wrappers) {
        Program p;
        Function f;
        f.name = "main";
        Statement current;
        current.kind = Statement::Kind::Conditional;
        current.condition_lhs =
            make_expression(Expression::Kind::Integer, {}, 1);
        current.condition_rhs =
            make_expression(Expression::Kind::Integer, {}, 1);
        for (unsigned i = 1; i < wrappers; ++i) {
            Statement outer;
            outer.kind = Statement::Kind::Conditional;
            outer.condition_lhs =
                make_expression(Expression::Kind::Integer, {}, 1);
            outer.condition_rhs =
                make_expression(Expression::Kind::Integer, {}, 1);
            outer.then_statements.push_back(std::move(current));
            current = std::move(outer);
        }
        f.statements.push_back(std::move(current));
        f.result = make_expression(Expression::Kind::Integer, {}, 0);
        p.functions.push_back(std::move(f));
        return p;
    };
    auto max_statement_depth = nested_conditionals(64);
    expect_rejection([&] { (void)emit_verified_llvm_ir(max_statement_depth); },
                     "ownership violation: unsupported conditional arm effect");
    auto excessive_statement_depth = nested_conditionals(65);
    expect_rejection([&] { (void)emit_verified_llvm_ir(excessive_statement_depth); },
                     "malformed native ownership AST: statement nesting exceeds 64");

    Program too_many_guarded_returns;
    Function guarded_limit_fn;
    guarded_limit_fn.name = "main";
    for (int i = 0; i < 9; ++i) {
        GuardedReturn guard;
        guard.lhs = make_expression(Expression::Kind::Integer, {}, 1);
        guard.rhs = make_expression(Expression::Kind::Integer, {}, 1);
        guard.result = make_expression(Expression::Kind::Integer, {}, i);
        guarded_limit_fn.early_returns.push_back(std::move(guard));
    }
    guarded_limit_fn.result = make_expression(Expression::Kind::Integer, {}, 0);
    too_many_guarded_returns.functions.push_back(std::move(guarded_limit_fn));
    expect_rejection([&] { (void)emit_verified_llvm_ir(too_many_guarded_returns); },
                     "malformed native ownership AST: guarded return count exceeds parser bound");

    Program too_many_conditional_cfgs;
    Function conditional_limit_fn;
    conditional_limit_fn.name = "main";
    for (int i = 0; i < 9; ++i) {
        Statement conditional;
        conditional.kind = Statement::Kind::Conditional;
        conditional.condition_lhs =
            make_expression(Expression::Kind::Integer, {}, 1);
        conditional.condition_rhs =
            make_expression(Expression::Kind::Integer, {}, 1);
        conditional_limit_fn.statements.push_back(std::move(conditional));
    }
    conditional_limit_fn.result =
        make_expression(Expression::Kind::Integer, {}, 0);
    too_many_conditional_cfgs.functions.push_back(
        std::move(conditional_limit_fn));
    expect_rejection([&] { (void)emit_verified_llvm_ir(too_many_conditional_cfgs); },
                     "malformed native ownership AST: conditional CFG count exceeds parser bound");

    Program oversized_conditional_arm;
    Function arm_limit_fn;
    arm_limit_fn.name = "main";
    Statement wide_conditional;
    wide_conditional.kind = Statement::Kind::Conditional;
    wide_conditional.condition_lhs =
        make_expression(Expression::Kind::Integer, {}, 1);
    wide_conditional.condition_rhs =
        make_expression(Expression::Kind::Integer, {}, 1);
    for (int i = 0; i < 17; ++i) {
        Statement local_stmt;
        local_stmt.kind = Statement::Kind::ScopedLocal;
        local_stmt.branch_local = std::make_unique<LocalBinding>(
            scalar_local("arm_" + std::to_string(i), i));
        wide_conditional.then_statements.push_back(std::move(local_stmt));
    }
    arm_limit_fn.statements.push_back(std::move(wide_conditional));
    arm_limit_fn.result = make_expression(Expression::Kind::Integer, {}, 0);
    oversized_conditional_arm.functions.push_back(std::move(arm_limit_fn));
    expect_rejection([&] { (void)emit_verified_llvm_ir(oversized_conditional_arm); },
                     "malformed native ownership AST: conditional arm count exceeds parser bound");

    Program malformed_array_commit_arity;
    Function array_commit_limit_fn;
    array_commit_limit_fn.name = "main";
    Statement array_commit;
    array_commit.kind = Statement::Kind::ArrayCommitFilled;
    array_commit.receiver = "owner";
    array_commit.array_commit_length =
        make_expression(Expression::Kind::Integer, {}, 3);
    for (int i = 0; i < 3; ++i)
        array_commit.array_commit_values.push_back(
            make_expression(Expression::Kind::Integer, {}, i));
    array_commit_limit_fn.statements.push_back(std::move(array_commit));
    array_commit_limit_fn.result =
        make_expression(Expression::Kind::Integer, {}, 0);
    malformed_array_commit_arity.functions.push_back(
        std::move(array_commit_limit_fn));
    expect_rejection([&] { (void)emit_verified_llvm_ir(malformed_array_commit_arity); },
                     "malformed native ownership AST: array commit fill count must equal four");

    Program oversized_caught_finally;
    Function caught_finally_limit_fn;
    caught_finally_limit_fn.name = "main";
    caught_finally_limit_fn.caught_throw_value =
        make_expression(Expression::Kind::Integer, {}, 1);
    caught_finally_limit_fn.catch_binding = "error";
    for (int i = 0; i < 17; ++i) {
        Statement cleanup;
        cleanup.kind = Statement::Kind::Defer;
        cleanup.receiver = "box";
        cleanup.field = "value";
        cleanup.expression =
            make_expression(Expression::Kind::Integer, {}, i);
        caught_finally_limit_fn.statements.push_back(std::move(cleanup));
    }
    caught_finally_limit_fn.result =
        make_expression(Expression::Kind::Integer, {}, 0);
    oversized_caught_finally.functions.push_back(
        std::move(caught_finally_limit_fn));
    expect_rejection([&] { (void)emit_verified_llvm_ir(oversized_caught_finally); },
                     "malformed native ownership AST: caught throw finally statement count exceeds parser bound");

    Program unknown_expression_kind;
    Function unknown_expression_fn;
    unknown_expression_fn.name = "main";
    unknown_expression_fn.result = make_expression(
        static_cast<Expression::Kind>(255));
    unknown_expression_kind.functions.push_back(std::move(unknown_expression_fn));
    expect_rejection([&] { (void)emit_verified_llvm_ir(unknown_expression_kind); },
                     "malformed native ownership AST: unknown expression kind");

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
    ClassField duplicate_x_first;
    duplicate_x_first.name = "x";
    duplicate_x_first.initializer_expression =
        make_expression(Expression::Kind::Integer, {}, 1);
    duplicate_x_first.initial = 1;
    duplicate_class.fields.push_back(std::move(duplicate_x_first));
    ClassField duplicate_x_second;
    duplicate_x_second.name = "x";
    duplicate_x_second.initializer_expression =
        make_expression(Expression::Kind::Integer, {}, 2);
    duplicate_x_second.initial = 2;
    duplicate_class.fields.push_back(std::move(duplicate_x_second));
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

    Program unknown_local_class;
    Function unknown_class_main;
    unknown_class_main.name = "main";
    LocalBinding unknown_class_binding;
    unknown_class_binding.name = "value";
    unknown_class_binding.new_class = true;
    unknown_class_binding.class_name = "Ghost";
    unknown_class_main.locals.push_back(std::move(unknown_class_binding));
    unknown_class_main.result = make_expression(Expression::Kind::Integer, {}, 0);
    unknown_local_class.functions.push_back(std::move(unknown_class_main));
    expect_rejection([&] { (void)emit_verified_llvm_ir(unknown_local_class); },
                     "unknown class 'Ghost'");

    Program unknown_move_source;
    ClassDecl empty_class;
    empty_class.name = "Box";
    ClassField box_value;
    box_value.name = "value";
    box_value.initializer_expression =
        make_expression(Expression::Kind::Integer, {}, 0);
    box_value.initial = 0;
    empty_class.fields.push_back(std::move(box_value));
    unknown_move_source.classes.push_back(std::move(empty_class));
    Function unknown_move_main;
    unknown_move_main.name = "main";
    LocalBinding moved_class;
    moved_class.name = "dst";
    moved_class.move_class = true;
    moved_class.class_name = "Box";
    moved_class.move_source = "missing";
    unknown_move_main.locals.push_back(std::move(moved_class));
    unknown_move_main.result = make_expression(Expression::Kind::Integer, {}, 0);
    unknown_move_source.functions.push_back(std::move(unknown_move_main));
    expect_rejection([&] { (void)emit_verified_llvm_ir(unknown_move_source); },
                     "unknown class move source 'missing'");

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
                     "statement contains fields incompatible with its kind");

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
                     "statement contains fields incompatible with its kind");

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
                     "statement contains fields incompatible with its kind");

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
                     "actor send is missing from source-order action plan");

    Program orphan_stop;
    Function stop_fn;
    stop_fn.name = "main";
    stop_fn.actor_stops.push_back("worker");
    stop_fn.result = make_expression(Expression::Kind::Integer, {}, 0);
    orphan_stop.functions.push_back(std::move(stop_fn));
    expect_rejection([&] { (void)emit_verified_llvm_ir(orphan_stop); },
                     "actor stop is missing from source-order action plan");

    Program duplicate_send_action;
    Function duplicate_schedule_fn;
    duplicate_schedule_fn.name = "main";
    ActorSend scheduled;
    scheduled.reference = "worker";
    scheduled.payload = make_expression(Expression::Kind::Integer, {}, 7);
    duplicate_schedule_fn.actor_sends.push_back(std::move(scheduled));
    duplicate_schedule_fn.actor_actions.push_back({ActorAction::Kind::Send, 0});
    duplicate_schedule_fn.actor_actions.push_back({ActorAction::Kind::Send, 0});
    duplicate_schedule_fn.result = make_expression(Expression::Kind::Integer, {}, 0);
    duplicate_send_action.functions.push_back(std::move(duplicate_schedule_fn));
    expect_rejection([&] { (void)emit_verified_llvm_ir(duplicate_send_action); },
                     "actor send action is duplicated");

    Program bad_action_index;
    Function indexed_fn;
    indexed_fn.name = "main";
    indexed_fn.actor_actions.push_back({ActorAction::Kind::Send, 0});
    indexed_fn.result = make_expression(Expression::Kind::Integer, {}, 0);
    bad_action_index.functions.push_back(std::move(indexed_fn));
    expect_rejection([&] { (void)emit_verified_llvm_ir(bad_action_index); },
                     "actor send action index is out of range");

    Program unknown_action_kind;
    Function unknown_action_fn;
    unknown_action_fn.name = "main";
    unknown_action_fn.actor_actions.push_back({
        static_cast<ActorAction::Kind>(255), 0});
    unknown_action_fn.result = make_expression(Expression::Kind::Integer, {}, 0);
    unknown_action_kind.functions.push_back(std::move(unknown_action_fn));
    expect_rejection([&] { (void)emit_verified_llvm_ir(unknown_action_kind); },
                     "unknown actor action kind");

    Program hidden_effect_bits;
    Function hidden_effect_fn;
    hidden_effect_fn.name = "main";
    hidden_effect_fn.declared_effects.bits =
        static_cast<std::uint32_t>(FunctionEffect::Alloc);
    hidden_effect_fn.result =
        make_expression(Expression::Kind::Integer, {}, 0);
    hidden_effect_bits.functions.push_back(std::move(hidden_effect_fn));
    expect_rejection([&] { (void)emit_verified_llvm_ir(hidden_effect_bits); },
                     "malformed native ownership AST: function contains hidden effect bits");

    Program invalid_effect_bits;
    Function invalid_effect_fn;
    invalid_effect_fn.name = "main";
    invalid_effect_fn.has_declared_effects = true;
    invalid_effect_fn.declared_effects.bits = (1u << 31);
    invalid_effect_fn.result =
        make_expression(Expression::Kind::Integer, {}, 0);
    invalid_effect_bits.functions.push_back(std::move(invalid_effect_fn));
    expect_rejection([&] { (void)emit_verified_llvm_ir(invalid_effect_bits); },
                     "malformed native ownership AST: function has invalid declared effect set");

    Program allocation_without_effect;
    Function allocation_effect_fn;
    allocation_effect_fn.name = "main";
    allocation_effect_fn.has_declared_effects = true;
    allocation_effect_fn.declared_effects.bits =
        static_cast<std::uint32_t>(FunctionEffect::StateRead);
    auto effect_owned = scalar_local("owned", 1);
    effect_owned.heap_owned = true;
    allocation_effect_fn.locals.push_back(std::move(effect_owned));
    allocation_effect_fn.result =
        make_expression(Expression::Kind::Integer, {}, 0);
    allocation_without_effect.functions.push_back(std::move(allocation_effect_fn));
    expect_rejection([&] { (void)emit_verified_llvm_ir(allocation_without_effect); },
                     "owned allocation violates using signature without alloc");

    Program direct_effect_guard_call;
    Function effect_helper;
    effect_helper.name = "helper";
    effect_helper.result =
        make_expression(Expression::Kind::Integer, {}, 1);
    direct_effect_guard_call.functions.push_back(std::move(effect_helper));
    Function effect_main;
    effect_main.name = "main";
    effect_main.has_declared_effects = true;
    effect_main.declared_effects.bits =
        static_cast<std::uint32_t>(FunctionEffect::Alloc);
    Statement effect_conditional;
    effect_conditional.kind = Statement::Kind::Conditional;
    effect_conditional.condition_lhs =
        make_expression(Expression::Kind::Integer, {}, 1);
    effect_conditional.condition_rhs =
        make_expression(Expression::Kind::Call, "helper");
    effect_main.statements.push_back(std::move(effect_conditional));
    effect_main.result =
        make_expression(Expression::Kind::Integer, {}, 0);
    direct_effect_guard_call.functions.push_back(std::move(effect_main));
    expect_rejection([&] { (void)emit_verified_llvm_ir(direct_effect_guard_call); },
                     "using effects require transitive call summaries");

    expect_rejection([&] {
        (void)parse_program(
            "pub fnc helper(): int { return 1; } "
            "pub fnc main(): int using alloc { "
            "if (1 == helper()) { } else { } fi "
            "return 0; }");
    }, "using effects require transitive call summaries");

    expect_rejection([&] {
        (void)parse_program(
            "pub fnc helper(): int { return 1; } "
            "pub fnc main(): int using alloc { "
            "if (1 == 1) { helper(); } else { } fi "
            "return 0; }");
    }, "using effects require transitive call summaries");

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
