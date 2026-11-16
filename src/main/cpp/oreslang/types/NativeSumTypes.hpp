#pragma once
#include "oreslang/frontend/Parser.hpp"
#include "oreslang/frontend/CallableSelector.hpp"
#include "oreslang/types/FunctorTypeRules.hpp"
#include <functional>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace oreslang {
inline std::string native_sum_scalar_name(ScalarType type) {
    switch (type) {
        case ScalarType::I64: return "int";
        case ScalarType::Bool: return "bool";
        case ScalarType::String: return "String";
    }
    throw std::runtime_error("invalid native sum scalar payload kind");
}
inline std::string native_sum_name(NativeSumType::Kind kind, ScalarType value, ScalarType error = ScalarType::I64) {
    // Invalid enum discriminants must never collapse to valid Result<int,int>
    // names, and unchecked ScalarType values must never turn into "int".
    const std::string component = native_sum_scalar_name(value);
    switch (kind) {
        case NativeSumType::Kind::Option:
            if (error != ScalarType::I64)
                throw std::runtime_error("native Option descriptor has noncanonical error slot");
            return "Option<" + component + ">";
        case NativeSumType::Kind::Result:
            return "Result<" + component + "," + native_sum_scalar_name(error) + ">";
    }
    throw std::runtime_error("invalid native sum descriptor kind");
}
inline void validate_native_sum_registry(const Program& program) {
    // Typed ASTs may originate from internal compiler stages rather than the
    // text parser. Both the resolver and the nominal type checker call this
    // before any descriptor can be referenced or emitted as an i64 handle.
    // NativeSumType is currently a scalar-only ABI descriptor. Validate
    // its canonical generic shape before any expression is admitted to IR.
    // The recursive functor rules never authorize native nested payload ABI.
    std::unordered_set<std::string> admitted_sum_names;
    const auto is_scalar = [](ScalarType kind) {
        return kind == ScalarType::I64 || kind == ScalarType::Bool || kind == ScalarType::String;
    };
    for (const auto& sum : program.sum_types) {
        if ((sum.kind != NativeSumType::Kind::Option && sum.kind != NativeSumType::Kind::Result) ||
                !is_scalar(sum.value_type) || !is_scalar(sum.error_type) ||
                (sum.kind == NativeSumType::Kind::Option && sum.error_type != ScalarType::I64))
            throw std::runtime_error("invalid native sum scalar ABI descriptor");
        if (!admitted_sum_names.insert(sum.name).second)
            throw std::runtime_error("duplicate native sum type descriptor");
        const auto expected = native_sum_name(sum.kind, sum.value_type, sum.error_type);
        const auto shape = types::functor_v1::parse_type(sum.name);
        const bool correct_kind = shape.name ==
            (sum.kind == NativeSumType::Kind::Option ? "Option" : "Result");
        if (!correct_kind || shape.str() != expected)
            throw std::runtime_error("native sum descriptor type-shape mismatch (nested payload requires owned ABI)");
    }
}
// Both compiler entrypoints accept internally generated ASTs, not just
// parser-vetted source. Validate pointer-bearing shapes before either pass
// can dereference an unchecked child or silently infer a scalar handle.
inline void validate_native_sum_expression_shape(const Expression& e) {
    using K = Expression::Kind;
    for (const auto& arg : e.arguments) {
        if (!arg) {
            if (e.kind == K::SumConstruct)
                throw std::runtime_error("malformed native sum constructor AST");
            if (e.kind == K::SumAccess || e.kind == K::ArrayCall)
                throw std::runtime_error("malformed native sum/member access AST");
            throw std::runtime_error("native expression has missing argument");
        }
    }
    if ((e.kind == K::ArrayCall || e.kind == K::SumAccess) && !e.left)
        throw std::runtime_error("malformed native sum/member access AST");
    if (e.kind == K::Call && !e.argument_names.empty() &&
            e.argument_names.size() != e.arguments.size())
        throw std::runtime_error("native call argument-name metadata arity mismatch");
    if (e.kind == K::Conditional) {
        if (!e.when_true || !e.when_false)
            throw std::runtime_error("native conditional has missing value branch");
        if ((e.condition && (e.condition_lhs || e.condition_rhs)) ||
            (!e.condition && (!e.condition_lhs || !e.condition_rhs)))
            throw std::runtime_error("native conditional requires exactly one complete condition");
    }
}
// Internal compiler ASTs do not inherit the source parser's pointer/arity
// guarantees. Check statement shapes before resolver or typechecker traversal.
inline void validate_native_sum_statement_shape(const Statement& item) {
    using K = Statement::Kind;
    if ((item.kind == K::ForEach || item.kind == K::ForIndex ||
         item.kind == K::ScopedLocal) && !item.branch_local)
        throw std::runtime_error("native scoped/iteration statement requires local binding");
    if (item.kind == K::ForEach && !item.expression)
        throw std::runtime_error("native for-of statement requires source expression");
    if (item.kind == K::ForIndex && (!item.branch_local->initializer ||
         !item.condition || !item.loop_step))
        throw std::runtime_error("native index loop requires initializer, condition and update");
    if (item.kind == K::Conditional &&
        ((item.condition && (item.condition_lhs || item.condition_rhs)) ||
         (!item.condition && (!item.condition_lhs || !item.condition_rhs))))
        throw std::runtime_error("native if statement requires exactly one complete condition");
    if (item.kind == K::ArrayCommitFilled) {
        if (!item.array_commit_length)
            throw std::runtime_error("native array commit requires checked length");
        for (const auto& value : item.array_commit_values)
            if (!value)
                throw std::runtime_error("native array commit has missing value");
    }
}
inline const NativeSumType& native_sum(const Program& program, const std::string& name) {
    for (const auto& type : program.sum_types) if (type.name == name) return type;
    throw std::runtime_error("unknown native sum type '" + name + "'");
}
// Module linking coalesces identical standard Option/Result shapes while
// rejecting conflicting ABI descriptions before the resolver sees them.
inline void merge_native_sum_descriptor(std::vector<NativeSumType>& target,
                                        const NativeSumType& incoming) {
    for (const auto& existing : target) {
        if (existing.name != incoming.name) continue;
        if (existing.kind != incoming.kind ||
                existing.value_type != incoming.value_type ||
                existing.error_type != incoming.error_type)
            throw std::runtime_error("conflicting linked native sum type descriptor '" + incoming.name + "'");
        return;
    }
    target.push_back(incoming);
}
// Context resolves constructors; the nominal type checker still independently
// validates every payload and use. This pass never turns arbitrary i64 into a sum.
inline void resolve_native_sums(Program& program) {
    validate_native_sum_registry(program);
    struct Hint {
        ScalarType scalar = ScalarType::I64;
        std::string sum{}, cls{};
        bool array = false;
        ScalarType element = ScalarType::I64;
        std::string element_class{};
    };
    using Env = std::unordered_map<std::string, Hint>;
    std::unordered_map<std::string, const ClassDecl*> classes;
    std::unordered_map<std::string, const Function*> functions;
    for (const auto& cls : program.classes) classes.emplace(cls.name, &cls);
    for (const auto& fn : program.functions) functions.emplace(native_function_slot(fn.name, fn.parameters.size()), &fn);
    auto field_hint = [](const ClassField& f) { return Hint{f.scalar_type, f.sum_type, f.class_name, f.int_array, f.array_element_type, f.array_record_class}; };
    auto local_hint = [](const LocalBinding& b) { return Hint{b.scalar_type, b.sum_type, b.class_name, b.int_array, b.array_element_type, b.array_record_class}; };
    auto add_type = [&](NativeSumType::Kind kind, ScalarType value, ScalarType error = ScalarType::I64) {
        const auto name = native_sum_name(kind, value, error);
        for (const auto& type : program.sum_types) if (type.name == name) return name;
        program.sum_types.push_back({kind, name, value, error}); return name;
    };
    std::function<Hint(Expression&, const Env&, const std::string&)> expression;
    expression = [&](Expression& e, const Env& env, const std::string& expected) -> Hint {
        validate_native_sum_expression_shape(e);
        using E = Expression::Kind;
        if (e.kind == E::SumConstruct) {
            if (e.trap_origin && (e.name != "Some" || e.arguments.size() != 1 ||
                    !native_trap_pure_integer(*e.arguments.front())))
                throw std::runtime_error("invalid native trap AST: operand must be a pure i64 expression");
            if (e.trap_origin && !expected.empty() && expected != "Option<int>")
                throw std::runtime_error("native trap requires exactly Option<int>, no nested/erased result conversion");
            // Do not admit a partially rewritten/malformed constructor AST.
            // The native lowering consumes only name + positional payloads;
            // stray children must not hide effectful or ownership-sensitive IR.
            if (e.left || e.right || e.condition || e.condition_lhs ||
                    e.condition_rhs || e.when_true || e.when_false ||
                    e.type_arguments_present || !e.type_arguments.empty() ||
                    (!e.argument_names.empty() && e.argument_names.size() != e.arguments.size()))
                throw std::runtime_error("malformed native sum constructor AST");
            for (const auto& label : e.argument_names)
                if (!label.empty())
                    throw std::runtime_error("malformed native sum constructor AST");
            for (const auto& payload : e.arguments)
                if (!payload)
                    throw std::runtime_error("malformed native sum constructor AST");
            std::string selected = expected;
            if (selected.empty()) {
                if (e.name != "Some" || e.arguments.size() != 1) throw std::runtime_error("native sum constructor requires a declared Option/Result context");
                const auto value = expression(*e.arguments[0], env, {});
                if (!value.sum.empty() || !value.cls.empty() || value.array) throw std::runtime_error("native sum compound payload requires ownership/lifetime ABI");
                selected = add_type(NativeSumType::Kind::Option, value.scalar);
            }
            const NativeSumType type = native_sum(program, selected);
            const bool option = type.kind == NativeSumType::Kind::Option;
            if (e.trap_origin && (!option || type.value_type != ScalarType::I64))
                throw std::runtime_error("native trap only lowers Option<int> with noncatching checked arithmetic");
            if ((option && e.name != "Some" && e.name != "None") || (!option && e.name != "Ok" && e.name != "Err"))
                throw std::runtime_error("native sum constructor variant does not match declared sum type");
            if (e.arguments.size() != (e.name == "None" ? 0u : 1u)) throw std::runtime_error("native sum constructor payload arity mismatch");
            // A scalar Result/Option must never carry an untyped or differently
            // typed payload. In particular, Err(7) is not Err<String>, and a
            // nominal/nested sum cannot be laundered into an i64 slot. The
            // current native runtime only owns scalar sum payloads; richer
            // generic/owned payloads require an independently verified ABI.
            for (auto& arg : e.arguments) {
                const Hint payload = expression(*arg, env, {});
                const ScalarType required = e.name == "Err" ? type.error_type : type.value_type;
                if (!payload.sum.empty() || !payload.cls.empty() || payload.array)
                    throw std::runtime_error("native sum compound payload requires ownership/lifetime ABI");
                if (payload.scalar != required)
                    throw std::runtime_error("native sum constructor payload type does not match declared variant");
            }
            e.sum_type = selected; return {ScalarType::I64, selected};
        }
        if (e.kind == E::Variable || e.kind == E::String ||
                e.kind == E::Boolean || e.kind == E::Integer) {
            // A scalar leaf cannot hide ignored calls, tagged payloads,
            // captures or evaluation branches. Reject before type inference
            // returns early and before LLVM emits code.
            if (e.left || e.right || e.condition || e.condition_lhs ||
                    e.condition_rhs || e.when_true || e.when_false ||
                    !e.arguments.empty() || !e.argument_names.empty() ||
                    !e.type_arguments.empty() || e.type_arguments_present)
                throw std::runtime_error("malformed native scalar leaf AST");
        }
        if (e.kind == E::Variable) return env.count(e.name) ? env.at(e.name) : Hint{};
        if (e.kind == E::String) return {ScalarType::String};
        if (e.kind == E::Boolean) return {ScalarType::Bool};
        if (e.kind == E::Integer) return {};
        if (e.kind == E::Field) {
            const auto owner = expression(*e.left, env, {});
            if (!owner.cls.empty() && classes.count(owner.cls))
                for (const auto& field : classes.at(owner.cls)->fields) if (field.name == e.name) return field_hint(field);
            return {};
        }
        if (e.kind == E::Call) {
            const auto found = functions.find(native_function_slot(e.name, e.arguments.size()));
            for (std::size_t i = 0; i < e.arguments.size(); ++i) {
                std::size_t slot = i;
                if (found != functions.end() && !e.argument_names.empty() && !e.argument_names[i].empty()) {
                    slot = 0; while (slot < found->second->parameters.size() && found->second->parameters[slot].name != e.argument_names[i]) ++slot;
                }
                const auto context = found != functions.end() && slot < found->second->parameters.size() ? found->second->parameters[slot].sum_type : std::string{};
                const auto actual = expression(*e.arguments[i], env, context);
                if (found != functions.end() && actual.sum != context)
                    throw std::runtime_error("native call argument cannot implicitly wrap or erase Option/Result");
            }
            if (found == functions.end()) return {};
            return {found->second->return_type, found->second->return_sum, found->second->return_class};
        }
        if (e.kind == E::RecordConstruct) {
            for (std::size_t i = 0; i < e.arguments.size(); ++i) {
                const ClassField* field = classes.count(e.name) ? constructor_field(*classes.at(e.name), i) : nullptr;
                expression(*e.arguments[i], env, field ? field->sum_type : std::string{});
            }
            return {ScalarType::I64, {}, e.name};
        }
        if (e.kind == E::ArrayCall || e.kind == E::SumAccess) {
            if (!e.left)
                throw std::runtime_error("malformed native sum/member access AST");
            for (const auto& argument : e.arguments)
                if (!argument)
                    throw std::runtime_error("malformed native sum/member access AST");
            const auto owner = expression(*e.left, env, {});
            std::vector<Hint> argument_types;
            argument_types.reserve(e.arguments.size());
            for (auto& arg : e.arguments) argument_types.push_back(expression(*arg, env, {}));
            if (e.kind == E::ArrayCall && e.name == "get_copy") {
                // Copy<int> is represented as an owned value. Do not turn an
                // Array<String>, class, nested sum, or foreign scalar into a
                // forged Option<int> handle. The bounds branch is native AOT.
                if (!owner.array || owner.element != ScalarType::I64 ||
                    !owner.element_class.empty() || argument_types.size() != 1 ||
                    !argument_types[0].sum.empty() ||
                    !argument_types[0].cls.empty() || argument_types[0].array ||
                    argument_types[0].scalar != ScalarType::I64)
                    throw std::runtime_error(
                        "native Array.get_copy requires Array<int> and a checked int index");
                const auto sum = add_type(NativeSumType::Kind::Option, ScalarType::I64);
                if (!expected.empty() && expected != sum)
                    throw std::runtime_error(
                        "native Array.get_copy result requires Option<int>, no implicit conversion");
                e.sum_type = sum;
                return {ScalarType::I64, sum};
            }
            if (!owner.sum.empty()) {
                const NativeSumType type = native_sum(program, owner.sum);
                const bool option = type.kind == NativeSumType::Kind::Option;
                const bool query = option ? e.name == "is_some" || e.name == "is_none" : e.name == "is_ok" || e.name == "is_err";
                const bool unwrap = e.name == "unwrap";
                const bool fallback = e.name == "unwrap_or";
                // expect(msg) is unwrap with a caller message (Java: panic "msg: <payload>").
                // The native slice prints a literal message only, so require a literal.
                const bool expect = e.name == "expect";
                if ((!query && !unwrap && !fallback && !expect) || e.arguments.size() != (fallback || expect ? 1u : 0u))
                    throw std::runtime_error("unsupported native sum method or arity '" + e.name + "'");
                if (expect && e.arguments[0]->kind != E::String)
                    throw std::runtime_error("native Option/Result.expect requires a string literal message");
                if (fallback) {
                    const auto& default_value = argument_types.front();
                    if (!default_value.sum.empty() || !default_value.cls.empty() ||
                            default_value.array || default_value.scalar != type.value_type)
                        throw std::runtime_error("native sum unwrap_or fallback type does not match payload");
                }
                e.kind = E::SumAccess; e.sum_type = owner.sum;
                e.resolved_type = query ? ScalarType::Bool : type.value_type;
                return {e.resolved_type};
            }
            return {owner.element, {}, owner.element_class};
        }
        if (e.kind == E::Conditional) {
            if (e.condition) expression(*e.condition, env, {});
            if (e.condition_lhs) expression(*e.condition_lhs, env, {});
            if (e.condition_rhs) expression(*e.condition_rhs, env, {});
            const auto yes = expression(*e.when_true, env, expected);
            const auto no = expression(*e.when_false, env, expected.empty() ? yes.sum : expected);
            // The conditional expression is one value. A scalar arm cannot
            // masquerade as an Option/Result descriptor, and two different
            // nominal sums cannot share an untyped i64 return slot.
            // Check both arms even when the surrounding context supplied a
            // sum type; nominal runtime checks are not a compiler type proof.
            if (!yes.sum.empty() || !no.sum.empty() || !expected.empty()) {
                if (yes.sum.empty() || no.sum.empty() || yes.sum != no.sum ||
                    (!expected.empty() && yes.sum != expected))
                    throw std::runtime_error(
                        "native conditional sum branches must resolve to the same declared sum type");
            }
            return yes;
        }
        Hint left, right;
        if (e.left) left = expression(*e.left, env, {});
        if (e.right) right = expression(*e.right, env, {});
        if ((e.kind == E::Add || e.kind == E::Subtract || e.kind == E::Multiply ||
                e.kind == E::Negate) && (!left.sum.empty() || !right.sum.empty()))
            throw std::runtime_error("native tagged Option/Result is not an integer arithmetic operand");
        for (auto& arg : e.arguments) expression(*arg, env, {});
        if (e.kind == E::LogicalNot || e.kind == E::Equal || e.kind == E::NotEqual || e.kind == E::Less || e.kind == E::LessEqual ||
                e.kind == E::Greater || e.kind == E::GreaterEqual || e.kind == E::LogicalAnd || e.kind == E::LogicalOr || e.kind == E::LogicalXor)
            return {ScalarType::Bool};
        return left;
    };
    for (auto& cls : program.classes) {
        Env env;
        for (std::size_t i = 0; i < cls.constructor_parameters.size(); ++i) {
            const auto* field = constructor_field(cls, i);
            env.emplace(cls.constructor_parameters[i], field ? field_hint(*field) : Hint{cls.constructor_parameter_types.empty() ? ScalarType::I64 : cls.constructor_parameter_types[i]});
        }
        for (auto& field : cls.fields) if (field.initializer_expression) expression(*field.initializer_expression, env, field.sum_type);
    }
    auto function = [&](Function& fn) {
        Env env;
        for (const auto& p : fn.parameters) env.emplace(p.name, Hint{p.scalar_type, p.sum_type, p.class_name});
        for (auto& binding : fn.locals) {
            if (binding.initializer) {
                const auto actual = expression(*binding.initializer, env, binding.sum_type);
                if (actual.sum != binding.sum_type)
                    throw std::runtime_error("native local Option/Result initializer cannot implicitly wrap or erase tagged value");
            }
            for (std::size_t i = 0; i < binding.constructor_arguments.size(); ++i) {
                const auto* field = classes.count(binding.class_name) ? constructor_field(*classes.at(binding.class_name), i) : nullptr;
                expression(*binding.constructor_arguments[i], env, field ? field->sum_type : std::string{});
            }
            env.emplace(binding.name, local_hint(binding));
        }
        std::function<void(std::vector<Statement>&, Env&)> statements;
        statements = [&](std::vector<Statement>& items, Env& scope) {
            for (auto& item : items) {
                validate_native_sum_statement_shape(item);
                if (item.branch_local && item.branch_local->initializer) expression(*item.branch_local->initializer, scope, item.branch_local->sum_type);
                if (item.kind == Statement::Kind::ForEach || item.kind == Statement::Kind::ForIndex) {
                    if (item.expression) expression(*item.expression, scope, {});
                    Env body = scope; body.emplace(item.branch_local->name, local_hint(*item.branch_local));
                    if (item.condition) expression(*item.condition, body, {});
                    if (item.loop_step) expression(*item.loop_step, body, {});
                    statements(item.then_statements, body); continue;
                }
                if (item.condition) expression(*item.condition, scope, {});
                if (item.condition_lhs) expression(*item.condition_lhs, scope, {});
                if (item.condition_rhs) expression(*item.condition_rhs, scope, {});
                std::string context = item.kind == Statement::Kind::Return ? fn.return_sum : std::string{};
                if (item.kind == Statement::Kind::ScalarWrite && scope.count(item.receiver))
                    context = scope.at(item.receiver).sum;
                if (!item.receiver.empty() && scope.count(item.receiver) && classes.count(scope.at(item.receiver).cls))
                    for (const auto& field : classes.at(scope.at(item.receiver).cls)->fields) if (field.name == item.field) context = field.sum_type;
                if (item.expression) {
                    const auto actual = expression(*item.expression, scope, context);
                    if (item.kind == Statement::Kind::Return && actual.sum != context)
                        throw std::runtime_error("native return cannot implicitly wrap or erase Option/Result");
                }
                if (item.array_commit_length) expression(*item.array_commit_length, scope, {});
                for (auto& value : item.array_commit_values) expression(*value, scope, {});
                Env yes = scope, no = scope;
                statements(item.then_statements, yes); statements(item.else_statements, no);
                if (item.kind == Statement::Kind::ScopedLocal) scope.emplace(item.branch_local->name, local_hint(*item.branch_local));
            }
        };
        statements(fn.statements, env); statements(fn.caught_throw_defers, env);
        for (auto& guard : fn.early_returns) {
            if (guard.condition) expression(*guard.condition, env, {});
            if (guard.lhs) expression(*guard.lhs, env, {});
            if (guard.rhs) expression(*guard.rhs, env, {});
            expression(*guard.result, env, fn.return_sum);
        }
        if (fn.result) {
            const auto actual = expression(*fn.result, env, fn.return_sum);
            if (actual.sum != fn.return_sum)
                throw std::runtime_error("native result cannot implicitly wrap or erase Option/Result");
        }
    };
    for (auto& fn : program.functions) function(fn);
    for (auto& actor : program.actors) function(actor.on_message);
}
}
