#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include "oreslang/types/FunctionEffects.hpp"
#include "oreslang/types/FuturePipelineContract.hpp"
#include "oreslang/types/NativeHasFutureContract.hpp"

namespace oreslang {

// Deliberately restricted native scalar expression AST. The source type system
// distinguishes int and bool even though the initial same-module ABI canonicalizes
// both into i64 slots/results; bool values are always normalized to 0 or 1.
enum class ScalarType { I64, Bool, String };

// Sum payloads are values, never untyped integer handles or class pointers.
struct NativeSumType {
    enum class Kind { Option, Result };
    Kind kind = Kind::Option;
    std::string name;
    ScalarType value_type = ScalarType::I64;
    ScalarType error_type = ScalarType::I64;
};

struct GenericTypeArgument {
    std::string name; // empty for positional arguments
    std::string type; // native stage 1 admits only int
    bool infer = false;
};
struct GenericParameter {
    std::string name;
    std::string default_type; // empty when required, int or earlier slot otherwise
};

struct Expression {
    enum class Kind {
        Integer, Boolean, String, Console, Variable,
        Negate, LogicalNot,
        Add, Subtract, Multiply, Remainder,
        Equal, NotEqual, Less, LessEqual, Greater, GreaterEqual,
        LogicalAnd, LogicalOr, LogicalXor,
        Conditional,
        Call, ArrayCall, RecordConstruct, SumConstruct, SumAccess, Copy, Take, Share, Borrow, BorrowMut, Field
    };
    Kind kind;
    std::size_t offset = 0;
    std::int64_t integer = 0;
    bool boolean = false;
    mutable ScalarType resolved_type = ScalarType::I64;
    mutable std::string resolved_class;
    std::string name;
    std::unique_ptr<Expression> left;
    std::unique_ptr<Expression> right;
    // Conditional is the native scalar form of Oreslang's lazy `? :`.
    // condition is a first-class bool expression. condition_lhs/condition_rhs
    // remain only as compatibility fields for the immediately preceding
    // equality-only ternary branch while stacked PRs are reviewed.
    std::unique_ptr<Expression> condition;
    std::unique_ptr<Expression> condition_lhs;
    std::unique_ptr<Expression> condition_rhs;
    std::unique_ptr<Expression> when_true;
    std::unique_ptr<Expression> when_false;
    std::vector<std::unique_ptr<Expression>> arguments;
    std::vector<std::string> argument_names; // indexed in lexical source order
    std::vector<GenericTypeArgument> type_arguments;
    bool type_arguments_present = false;
    mutable std::string sum_type;
};

// Returns each lexical source argument's checked declaration slot for native
// Array<T> builtins. Only these runtime-owned methods have stable, statically
// known parameter names; this is not dynamic member-name dispatch.
inline std::vector<std::size_t> native_array_argument_slots(const Expression& expr) {
    if (expr.kind != Expression::Kind::ArrayCall)
        throw std::runtime_error("native array binder requires a member call");
    if (expr.type_arguments_present || !expr.type_arguments.empty())
        throw std::runtime_error(
            "native Array methods do not accept generic type arguments");
    const bool get = expr.name == "get";
    const bool add = expr.name == "add";
    const bool set = expr.name == "set";
    if (!(get || add || set))
        throw std::runtime_error(
            "native Array<int> effect supports only add(value) or set(index,value); "
            "scalar expression supports only get(index)");
    const std::size_t expected = set ? 2 : 1;
    if (expr.arguments.size() != expected)
        throw std::runtime_error("native Array method argument arity mismatch");
    if (!expr.argument_names.empty() &&
        expr.argument_names.size() != expr.arguments.size())
        throw std::runtime_error("invalid native member argument layout");
    std::vector<bool> filled(expected, false);
    std::vector<std::size_t> source_to_formal;
    source_to_formal.reserve(expected);
    bool saw_named = false;
    std::size_t positional = 0;
    for (std::size_t i = 0; i < expected; ++i) {
        const std::string label = expr.argument_names.empty()
            ? std::string{} : expr.argument_names[i];
        std::size_t slot = positional;
        if (!label.empty()) {
            saw_named = true;
            if (label == "index" && (get || set)) slot = 0;
            else if (label == "value" && add) slot = 0;
            else if (label == "value" && set) slot = 1;
            else throw std::runtime_error(
                "unknown named native array argument '" + label +
                "' for '" + expr.name + "'");
        } else {
            if (saw_named)
                throw std::runtime_error(
                    "positional member argument cannot follow named argument");
            ++positional;
        }
        if (slot >= expected || filled[slot])
            throw std::runtime_error(
                "duplicate native array argument binding for '" + expr.name + "'");
        filled[slot] = true;
        source_to_formal.push_back(slot);
    }
    for (bool used : filled)
        if (!used) throw std::runtime_error("incomplete native array argument binding");
    return source_to_formal;
}

struct Parameter {
    enum class Mode { Value, Borrow, BorrowMut };
    std::string name;
    std::string class_name; // empty means scalar int/bool
    std::string generic_type; // stage-one i64 generic slot
    std::string pending_type_alias;
    ScalarType scalar_type = ScalarType::I64;
    Mode mode = Mode::Value;
    std::string sum_type;
};

// Immutable, lexical i64 slot with initializer-before-publication semantics.
struct LocalBinding {
    std::string name;
    std::unique_ptr<Expression> initializer;
    bool heap_owned = false;
    bool int_array = false; // non-Copy owned i64-slot array local, no escape
    ScalarType array_element_type = ScalarType::I64; // int value or checked immutable String handle
    std::string array_record_class;
    bool record_view = false; // class-valued initializer: leaf view or uniquely owned array-containing call result
    std::string class_name; // only stack-constructed, bounded multi-field i64 class
    // Declaration-site and new-expression type slots, bound independently.
    // The initial native monomorphization target accepts int representations only.
    std::vector<GenericTypeArgument> class_type_arguments;
    std::vector<GenericTypeArgument> constructor_type_arguments;
    bool new_class = false;
    // Pure scalar constructor arguments, evaluated once at call site.
    std::vector<std::unique_ptr<Expression>> constructor_arguments;
    // Source lexical labels, one per actual, empty for a positional prefix.
    std::vector<std::string> constructor_argument_names;
    // Restricted local stack-only explicit class move.
    bool move_class = false;
    std::string move_source;
    bool owner_mutable = false; // explicit let mut; required for exclusive class loan
    bool field_mutable = false; // actor-owned fields only
    ScalarType scalar_type = ScalarType::I64;
    std::string pending_type_alias; // transparent compile-time alias identity
    std::size_t offset = 0;
    std::string sum_type;
    // Nonempty only for the first capture-free source Task<int> slice.
    // The binding receives the result of exactly one scheduler-owned Task await.
    std::string task_await_name;
    // Nonempty only for the first checked Future<int> pipeline slice. The
    // binding receives the result of exactly one scheduler-owned Future await.
    std::string future_await_name;
    bool assignable = false; // mutable lexical slot, distinct from referent capability
};

struct ClassField {
    std::string name;
    std::int64_t initial = 0;
    std::unique_ptr<Expression> initializer_expression;
    bool public_member = false;
    ScalarType scalar_type = ScalarType::I64;
    std::string class_name; // unique inline nested class, never a raw handle
    bool int_array = false; // owned opaque handle; never exposed as an int
    bool final_field = false; // fixed field slot: writes rejected by ownership checker
    bool mutable_referent = false; // val aliases final mut; independent of slot fixedness
    bool constructor_argument = false; // declaration-only field supplied by implicit positional constructor
    ScalarType array_element_type = ScalarType::I64;
    std::string array_record_class;
    std::string sum_type;
};

struct ClassMethod {
    std::string name;
    std::size_t arity = 0; // source arguments, excludes hidden self
    bool public_member = false;
    bool mutable_receiver = false;
};

struct ClassDecl {
    bool exported = false;
    // Source-level generic names are nominal slot identities. Native v1
    // permits only the int specialization; no erased dynamic layouts.
    std::vector<GenericParameter> generic_parameters;
    std::vector<ClassMethod> methods;
    std::string name;
    // Up to 16 scalar i64 or owned Array<int> fields.
    // The ordered vector defines the native LLVM struct layout.
    std::vector<ClassField> fields;
    bool explicit_constructor = false;
    bool constructor_exported = false;
    std::vector<std::string> constructor_parameters;
    std::vector<ScalarType> constructor_parameter_types;
};

// Resolve a nominal generic specialization to declaration order, before any
// ownership mutation or LLVM codegen. All representations are int in v1.
// Never treat the textual order of named arguments as nominal ABI identity.
inline std::vector<std::string> native_scalar_class_generic_slots(
        const ClassDecl& cls, const std::vector<GenericTypeArgument>& args) {
    const std::size_t count = cls.generic_parameters.size();
    if (count == 0) {
        if (!args.empty())
            throw std::runtime_error("non-generic native class cannot accept type arguments");
        return {};
    }
    if (args.size() > count)
        throw std::runtime_error("too many native class generic arguments");
    std::vector<std::string> slots(count);
    std::vector<bool> assigned(count, false);
    std::size_t positional = 0;
    bool saw_named = false;
    for (const auto& arg : args) {
        std::size_t target = positional;
        if (!arg.name.empty()) {
            saw_named = true;
            target = 0;
            while (target < count && cls.generic_parameters[target].name != arg.name)
                ++target;
            if (target == count)
                throw std::runtime_error("unknown named native class generic '" + arg.name + "'");
        } else {
            if (saw_named)
                throw std::runtime_error("positional class generic argument after named argument");
            ++positional;
        }
        if (target >= count || assigned[target])
            throw std::runtime_error("duplicate native class generic argument");
        assigned[target] = true;
        if (arg.infer)
            throw std::runtime_error("native class generic inference holes require typed constructor inference");
        if (arg.type != "int")
            throw std::runtime_error("native generic class specialization supports only int");
        slots[target] = "int";
    }
    for (std::size_t i = 0; i < count; ++i) {
        if (assigned[i]) continue;
        const std::string& value = cls.generic_parameters[i].default_type;
        if (value == "int") {
            slots[i] = "int";
        } else {
            const auto dependency = std::find_if(
                cls.generic_parameters.begin(), cls.generic_parameters.begin() + i,
                [&](const GenericParameter& candidate) { return candidate.name == value; });
            if (dependency == cls.generic_parameters.begin() + i ||
                    slots[static_cast<std::size_t>(dependency - cls.generic_parameters.begin())] != "int")
                throw std::runtime_error("cannot infer native class generic parameter '" +
                                         cls.generic_parameters[i].name + "'");
            slots[i] = "int";
        }
    }
    return slots;
}

inline void native_verify_local_class_specialization(
        const LocalBinding& local, const ClassDecl& cls) {
    if (local.class_name != cls.name)
        throw std::runtime_error("native class generic owner identity mismatch");
    const auto declared = native_scalar_class_generic_slots(cls, local.class_type_arguments);
    if (local.new_class) {
        const auto created = native_scalar_class_generic_slots(
            cls, local.constructor_type_arguments);
        if (declared != created)
            throw std::runtime_error("native class generic declared/constructed type mismatch");
    } else if (!local.constructor_type_arguments.empty()) {
        throw std::runtime_error("native non-constructor has unexpected generic type arguments");
    }
}

inline const ClassField* constructor_field(const ClassDecl& cls, std::size_t parameter) {
    if (parameter >= cls.constructor_parameters.size()) return nullptr;
    for (const auto& field : cls.fields)
        if (field.constructor_argument && field.name == cls.constructor_parameters[parameter]) return &field;
    return nullptr;
}

// Statically resolve local new-Class constructor call-site labels against
// the declared constructor ABI slots, without evaluating source arguments.
// Only true local class construction uses this binder (not arbitrary calls).
inline std::vector<std::size_t> native_constructor_argument_slots(
        const LocalBinding& binding, const ClassDecl& cls) {
    if (!binding.new_class || binding.class_name != cls.name)
        throw std::runtime_error("native constructor binding requires exact class");
    const std::size_t arity = cls.constructor_parameters.size();
    if (binding.constructor_arguments.size() != arity)
        throw std::runtime_error("native class constructor argument arity mismatch");
    if (!binding.constructor_argument_names.empty() &&
        binding.constructor_argument_names.size() != arity)
        throw std::runtime_error("invalid native constructor label layout");
    std::vector<bool> assigned(arity, false);
    std::vector<std::size_t> mapping;
    mapping.reserve(arity);
    std::size_t positional = 0;
    bool saw_named = false;
    for (std::size_t source = 0; source < arity; ++source) {
        const std::string label = binding.constructor_argument_names.empty()
            ? std::string{} : binding.constructor_argument_names[source];
        std::size_t target = positional;
        if (!label.empty()) {
            saw_named = true;
            target = 0;
            while (target < arity &&
                   cls.constructor_parameters[target] != label) ++target;
            if (target == arity)
                throw std::runtime_error(
                    "unknown named native constructor argument '" + label + "'");
        } else {
            if (saw_named)
                throw std::runtime_error(
                    "positional constructor argument cannot follow named argument");
            ++positional;
        }
        if (target >= arity || assigned[target])
            throw std::runtime_error(
                "duplicate native constructor argument binding");
        assigned[target] = true;
        mapping.push_back(target);
    }
    for (bool used : assigned)
        if (!used)
            throw std::runtime_error("incomplete native constructor argument binding");
    return mapping;
}

// Native source value categories. Future<T> remains a no-IR boundary
// until the compiler can lower nonblocking pinned continuations.
struct SourceType {
    enum class Kind { I64, ActorRef, Future };
    Kind kind = Kind::I64;
    std::string argument;
};

struct ActorSpawnBinding {
    std::string name;
    std::string actor_type;
    SourceType declared_type;
    std::size_t offset = 0;
};

struct SourceTaskI64Binding {
    std::string name;
    std::unique_ptr<Expression> result;
    std::size_t offset = 0;
    bool awaited = false;
};

struct SourceFuturePipelineI64Binding {
    std::string name;
    std::string producer_function;
    std::vector<std::string> stages;
    std::vector<types::FuturePipelinePlan> plans;
    std::size_t offset = 0;
    bool root_consumed = false;
    bool awaited = false;
    // Set only after the complete ownership pass succeeds for the compilation
    // unit. Codegen must never consume a merely type-staged plan.
    bool ownership_admitted = false;
    // Deterministic compiler-only snapshot of the exact producer/stage/plan
    // shape authorized by ownership. Any post-admission rewrite must change
    // this value and therefore lose lowering authority.
    std::uint64_t ownership_authority_fingerprint = 0;
};

inline std::uint64_t future_pipeline_authority_fingerprint(
        const SourceFuturePipelineI64Binding& pipeline) noexcept {
    // FNV-1a is used only as a compact stale-proof detector inside one
    // compilation. This is not a cryptographic capability or persisted ABI.
    std::uint64_t hash = UINT64_C(1469598103934665603);
    const auto byte = [&](std::uint8_t value) {
        hash ^= value;
        hash *= UINT64_C(1099511628211);
    };
    const auto u64 = [&](std::uint64_t value) {
        for (unsigned shift = 0; shift < 64; shift += 8)
            byte(static_cast<std::uint8_t>(value >> shift));
    };
    const auto text = [&](std::string_view value) {
        u64(static_cast<std::uint64_t>(value.size()));
        for (unsigned char ch : value) byte(ch);
    };

    const auto auto_type = [&](const auto& self,
                               const types::AutoType& type) -> void {
        byte(static_cast<std::uint8_t>(type.kind));
        byte(type.shareable_domain ? 1u : 0u);
        byte(type.complete_nominal_layout ? 1u : 0u);
        u64(static_cast<std::uint64_t>(type.parameters.size()));
        for (const auto& parameter : type.parameters)
            self(self, parameter);
        u64(static_cast<std::uint64_t>(type.dyn_bounds.size()));
        for (const auto trait : type.dyn_bounds)
            byte(static_cast<std::uint8_t>(trait));
        u64(static_cast<std::uint64_t>(type.negative.size()));
        for (const auto trait : type.negative)
            byte(static_cast<std::uint8_t>(trait));
    };

    text(pipeline.name);
    text(pipeline.producer_function);
    u64(static_cast<std::uint64_t>(pipeline.offset));
    byte(pipeline.root_consumed ? 1u : 0u);
    byte(pipeline.awaited ? 1u : 0u);
    u64(static_cast<std::uint64_t>(pipeline.stages.size()));
    for (const auto& stage : pipeline.stages) text(stage);
    u64(static_cast<std::uint64_t>(pipeline.plans.size()));
    for (const auto& plan : pipeline.plans) {
        u64(plan.declaration_identity);
        u64(plan.pipeline_node_identity);
        auto_type(auto_type, plan.input_type);
        auto_type(auto_type, plan.output_type);
        u64(static_cast<std::uint64_t>(plan.prior_effects.bits));
        u64(static_cast<std::uint64_t>(plan.stage_effects.bits));
        u64(static_cast<std::uint64_t>(plan.cumulative_effects.bits));
        byte(static_cast<std::uint8_t>(plan.effect_provenance));
        byte(static_cast<std::uint8_t>(plan.input_ownership));
        byte(static_cast<std::uint8_t>(plan.produced_future_ownership));
        byte(static_cast<std::uint8_t>(plan.produced_future_provenance));
        byte(plan.flattens_future ? 1u : 0u);
        byte(plan.root_stage ? 1u : 0u);
    }
    // Reserve zero for "never authorized".
    return hash == 0 ? UINT64_C(1) : hash;
}

struct ActorSend {
    std::string reference;
    std::unique_ptr<Expression> payload;
    std::size_t offset = 0;
};
// Preserve the sequence of source-visible actor effects. Separating sends
// from stops changes program behavior and can hide use-after-stop errors.
struct ActorAction {
    enum class Kind { Send, Stop };
    Kind kind;
    std::size_t index = 0;
};

struct ActorFieldWrite {
    std::string field;
    std::unique_ptr<Expression> expression;
    std::size_t offset = 0;
};

struct Statement {
    enum class Kind { Call, FieldWrite, ScalarWrite, Defer, Conditional, ScopedLocal, ArrayCommitFilled, ForEach, ForIndex, Return };
    Kind kind;
    // Scoped scalar or readonly leaf record. ForEach uses an implicit element
    // binding (no initializer); expression is its named array source and
    // then_statements is the body. No owning scoped class drop is implied.
    std::unique_ptr<LocalBinding> branch_local;
    std::unique_ptr<Expression> expression;
    std::string receiver;
    std::string field;
    // ForIndex: branch_local initializer, condition and checked next value.
    std::unique_ptr<Expression> loop_step;
    // Dedicated compiler intrinsic, never lowered as a guest-callable
    // function: rt array_commit_filled(class_owner, length, v0, v1, v2, v3).
    // Exactly four Array<int> fields in class declaration order.
    std::unique_ptr<Expression> array_commit_length;
    std::vector<std::unique_ptr<Expression>> array_commit_values;
    std::size_t offset = 0;
    // A bounded synchronous if/else statement. Arms contain checked field
    // writes or direct calls; local declarations and suspension fail closed.
    std::unique_ptr<Expression> condition;
    std::unique_ptr<Expression> condition_lhs; // stacked equality-only compatibility
    std::unique_ptr<Expression> condition_rhs;
    std::vector<Statement> then_statements;
    std::vector<Statement> else_statements;
};

// Side-effect-free equality guard with return-only taken branch.
// All locals and ordinary statements precede guards; branch-local objects
// and suspension remain unsupported until path-sensitive cleanup is proven.
struct GuardedReturn {
    std::unique_ptr<Expression> condition;
    std::unique_ptr<Expression> lhs; // stacked equality-only compatibility
    std::unique_ptr<Expression> rhs;
    std::unique_ptr<Expression> result;
    std::size_t offset = 0;
};

// Experimental LLVM AOT-only, single-owner scalar loop frame. The grammar
// admits only one val i64 counter, a constant upper bound and a +1 body.
// No borrowed pointer, object owner, dynamic call or cleanup crosses a yield.
struct ScalarWhileLoop {
    std::string counter;
    std::int64_t initial = 0;
    std::int64_t upper_bound = 0;
};

struct Function {
    std::string name;
    std::vector<GenericParameter> generic_parameters;
    std::unique_ptr<ScalarWhileLoop> scalar_while;
    std::vector<Parameter> parameters;
    std::vector<LocalBinding> locals;
    std::vector<Statement> statements;
    std::vector<GuardedReturn> early_returns;
    std::vector<ActorSpawnBinding> actor_spawns;
    // First executable source Task slice: one capture-free Task<int> in trusted
    // zero-argument main, submitted before ordinary locals and awaited once.
    std::vector<SourceTaskI64Binding> source_tasks;
    // First executable Future-aware value-pipeline slice. Async producers are
    // compiled as scheduler work; sync unary stages execute on that carrier.
    std::vector<SourceFuturePipelineI64Binding> future_pipelines;
    bool async_source_i64 = false;
    /* Restricted lifecycle-await slice: exactly one actor lifecycle suspension. */
    std::string ready_await_actor;
    std::size_t ready_await_offset = 0;
    std::string done_await_actor;
    std::size_t done_await_offset = 0;
    // Records the syntactic protocol witness; codegen receives only a
    // checked SourceActorReadyControl resolution, not a magic name.
    NativeAwaitForm ready_await_form = NativeAwaitForm::None;
    // Non-Copy lexical Future<void> from a previously spawned actor's
    // get_ready(). The compiler consumes it into the existing CONTROL
    // continuation; it is never represented as an untyped i64 guest value.
    std::string ready_future_name;
    std::string ready_future_actor;
    std::vector<ActorSend> actor_sends;
    std::vector<std::string> actor_stops;
    std::vector<ActorAction> actor_actions; // source order of send/stop
    std::vector<ActorFieldWrite> actor_field_writes;
    bool returns_void = false;
    ScalarType return_type = ScalarType::I64;
    std::string pending_return_type_alias;
    std::string return_class; // immutable leaf record handle; never a stack pointer
    // Single-frame native i64 throw; no nested exceptions or suspension.
    std::unique_ptr<Expression> caught_throw_value;
    std::string catch_binding;
    std::vector<Statement> caught_throw_defers;
    std::unique_ptr<Expression> result;
    std::size_t offset = 0;
    bool exported = false;
    std::string method_owner; // compiler-owned class method privilege, never source syntax
    bool native_internal = false; // linker-owned private imported implementation
    FunctionEffects declared_effects{};
    bool has_declared_effects = false;
    std::string return_sum;
};

/** Frozen scheduler tier, distinct from ActorGroup-wide parallelism. */
enum class MaxActorThreads : std::uint8_t { LOW = 1, MEDIUM = 2, HIGH = 3 };

struct ActorDecl {
    std::string name;
    std::vector<LocalBinding> fields; // bounded, actor-confined integer slots
    Function on_message; // exactly one synchronous pub on_message(int): int
    std::size_t offset = 0;
    MaxActorThreads max_actor_threads = MaxActorThreads::LOW;
};

// Compiler-only structural contract evidence for the native i64 subset.
// Contract declarations do not allocate runtime objects or generate LLVM IR.
struct ContractMethod {
    std::string name;
    std::vector<Parameter::Mode> parameter_modes;
};

struct ModuleContract {
    std::string name;
    std::vector<ContractMethod> methods;
};

// Compiler-only type declarations. Neither traits nor interfaces nor
// transparent scalar aliases create runtime values, LLVM globals or
// constructors. The native scalar backend records the distinct source kinds
// without claiming full generic/typeclass or cross-unit linker support.
struct TypeOnlyDecl {
    enum class Kind { Alias, Trait, Interface };
    struct Constraint {
        enum class Kind { MinInclusive, MaxInclusive };
        Kind kind = Kind::MinInclusive;
        std::int64_t value = 0;
    };
    struct Method {
        std::string name;
        std::vector<std::string> parameter_types;
        std::string return_type;
    };
    Kind kind = Kind::Alias;
    std::string name;
    std::string module_name; // empty for root; captured at declaration site
    bool exported = false;
    std::string alias_target; // native v1 accepts int/bool or another alias
    std::vector<Constraint> constraints; // canonical min then max, compiler-only
    std::vector<std::string> parents;
    std::vector<Method> methods;
};

// First AOT-only resumable source slice. No owned borrows, calls, FFI,
// lexical cleanup or actor scheduler integration is admitted yet.
// The host supplies a zeroed, single-owner two-i64 continuation frame.
struct CooperativeScalarLoop {
    std::string name;
    std::string variable;
    std::int64_t initial = 0;
    std::size_t offset = 0;
};

// Native generator v1 deliberately starts with a value-only, zero-capture
// Iterator<int> activation. Each activation is represented by a caller-owned,
// pinned frame in LLVM IR; no native stack or tracing-GC object survives a pull.
struct ScalarGenerator {
    std::string name;
    std::vector<std::int64_t> yields;
    std::size_t offset = 0;
    bool exported = false;
};

struct ModuleImport {
    bool selected_class = false;
    std::string selected_module;
    std::string local_name;
    std::string path;
    std::size_t offset = 0;
};

struct TypeConstraintDescriptor {
    std::string schema_id;
    std::string qualified_type_name;
    std::string representation;
    std::string canonical;
    std::string fingerprint;
    bool validation_required = true;
};

struct Program {
    std::vector<ModuleImport> imports;
    std::vector<CooperativeScalarLoop> cooperative_loops;
    std::vector<ScalarGenerator> generators;
    std::string namespace_name;
    std::string module;
    std::vector<ClassDecl> classes;
    std::vector<ActorDecl> actors;
    std::vector<Function> functions;
    std::vector<ModuleContract> contracts;
    std::vector<TypeOnlyDecl> type_only_declarations;
    std::vector<TypeConstraintDescriptor> type_constraint_descriptors;
    std::vector<std::string> conforms;
    std::vector<NativeSumType> sum_types;
};

// Parse a constrained native subset: i64 expressions, call-scoped borrows and
// bounded multi-field i64 classes and trusted, synchronous actor send/stop.
// Actors remain incompatible with wasm32. The only admitted source suspension
// is one zero-local actor lifecycle await in trusted zero-argument main:
// direct/typed get_ready readiness or explicit ActorRef.get_done().
Program parse_program(std::string_view source, bool experimental_owned_int = false,
                      bool experimental_rt_scalar_loop = false);

// Internal module-linking stages: syntax trees are NOT admitted for codegen.
// resolve_and_validate_program must run after all module names are resolved.
Program parse_unresolved_program(std::string_view source, bool experimental_owned_int = false,
                                 bool experimental_rt_scalar_loop = false);
void resolve_and_validate_program(Program& program);

} // namespace oreslang
