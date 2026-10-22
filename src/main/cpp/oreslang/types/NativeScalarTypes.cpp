#include "oreslang/types/NativeScalarTypes.hpp"
#include "oreslang/frontend/CallableSelector.hpp"
#include <functional>
#include <stdexcept>
#include <unordered_map>

namespace oreslang {
namespace {
enum class Kind { Int, Bool, Class, Array, Void };
struct Type {
    Kind kind = Kind::Int;
    std::string name;
    bool operator==(const Type& other) const { return kind == other.kind && name == other.name; }
};
using Locals = std::unordered_map<std::string, Type>;
Type scalar(bool boolean) { return {boolean ? Kind::Bool : Kind::Int, {}}; }
Type parameter_type(const Parameter& p) {
    return p.class_name.empty() ? scalar(p.scalar_type == ScalarType::Bool) : Type{Kind::Class, p.class_name};
}
Type local_type(const LocalBinding& local) {
    if (local.int_array) return {Kind::Array, {}};
    return local.class_name.empty() ? scalar(local.scalar_type == ScalarType::Bool) : Type{Kind::Class, local.class_name};
}
[[noreturn]] void fail(std::size_t offset, const std::string& detail) {
    throw std::runtime_error("native scalar type mismatch: " + detail + " at byte " + std::to_string(offset));
}
void require(Type actual, Type expected, std::size_t offset, const std::string& detail) {
    if (!(actual == expected)) fail(offset, detail);
}
}
void validate_native_scalar_types(const Program& program) {
    std::unordered_map<std::string, const ClassDecl*> classes;
    std::unordered_map<std::string, const Function*> functions;
    for (const auto& cls : program.classes) classes.emplace(cls.name, &cls);
    for (const auto& fn : program.functions)
        functions.emplace(native_function_slot(fn.name, fn.parameters.size()), &fn);
    auto field_type = [&](Type owner, const std::string& name, std::size_t offset) -> Type {
        if (owner.kind == Kind::Array && name == "size") return scalar(false);
        if (owner.kind != Kind::Class) fail(offset, "field needs a nominal class");
        const auto cls = classes.find(owner.name);
        if (cls == classes.end()) fail(offset, "unknown class");
        for (const auto& field : cls->second->fields)
            if (field.name == name) return field.int_array ? Type{Kind::Array, {}} : scalar(field.scalar_type == ScalarType::Bool);
        fail(offset, "unknown class field");
    };
    std::function<Type(const Expression&, const Locals&)> expression;
    expression = [&](const Expression& e, const Locals& locals) -> Type {
        using E = Expression::Kind;
        switch (e.kind) {
            case E::Integer: return scalar(false);
            case E::Boolean: return scalar(true);
            case E::Variable: return locals.at(e.name);
            case E::Field: return field_type(expression(*e.left, locals), e.name, e.offset);
            case E::Copy: case E::Take: case E::Share: case E::Borrow: case E::BorrowMut:
                return expression(*e.left, locals);
            case E::Negate:
                require(expression(*e.left, locals), scalar(false), e.offset, "unary - requires int");
                return scalar(false);
            case E::LogicalNot:
                require(expression(*e.left, locals), scalar(true), e.offset, "! requires bool");
                return scalar(true);
            case E::Equal: case E::NotEqual: {
                auto left = expression(*e.left, locals), right = expression(*e.right, locals);
                if (!(left == right) || (left.kind != Kind::Int && left.kind != Kind::Bool)) fail(e.offset, "equality operand types");
                return scalar(true);
            }
            case E::Less: case E::LessEqual: case E::Greater: case E::GreaterEqual:
                require(expression(*e.left, locals), scalar(false), e.offset, "ordered comparison requires int");
                require(expression(*e.right, locals), scalar(false), e.offset, "ordered comparison requires int");
                return scalar(true);
            case E::LogicalAnd: case E::LogicalOr: case E::LogicalXor:
                require(expression(*e.left, locals), scalar(true), e.offset, "logical operator requires bool");
                require(expression(*e.right, locals), scalar(true), e.offset, "logical operator requires bool");
                return scalar(true);
            case E::Conditional: {
                if (e.condition) require(expression(*e.condition, locals), scalar(true), e.offset, "conditional needs bool");
                else {
                    auto lhs = expression(*e.condition_lhs, locals), rhs = expression(*e.condition_rhs, locals);
                    require(lhs, rhs, e.offset, "conditional equality types");
                }
                auto lhs = expression(*e.when_true, locals), rhs = expression(*e.when_false, locals);
                require(lhs, rhs, e.offset, "conditional arm types");
                return lhs;
            }
            case E::Add: case E::Subtract: case E::Multiply:
                require(expression(*e.left, locals), scalar(false), e.offset, "arithmetic requires int");
                require(expression(*e.right, locals), scalar(false), e.offset, "arithmetic requires int");
                return scalar(false);
            case E::ArrayCall:
                require(expression(*e.left, locals), {Kind::Array, {}}, e.offset, "array receiver");
                for (const auto& arg : e.arguments)
                    require(expression(*arg, locals), scalar(false), arg->offset, "Array<int> argument requires int");
                return scalar(false); // existing native array ABI returns an i64 status/value
            case E::Call: {
                const auto& fn = *functions.at(native_function_slot(e.name, e.arguments.size()));
                std::size_t positional = 0;
                for (std::size_t i = 0; i < e.arguments.size(); ++i) {
                    std::size_t slot = positional++;
                    if (!e.argument_names.empty() && !e.argument_names[i].empty()) {
                        slot = 0;
                        while (slot < fn.parameters.size() && fn.parameters[slot].name != e.argument_names[i]) ++slot;
                    }
                    require(expression(*e.arguments[i], locals), parameter_type(fn.parameters.at(slot)),
                            e.arguments[i]->offset, "argument bool/int/nominal type differs from parameter");
                }
                return fn.returns_void ? Type{Kind::Void, {}} : scalar(fn.return_type == ScalarType::Bool);
            }
        }
        fail(e.offset, "unsupported expression");
    };
    auto compare = [&](const Expression& lhs, const Expression& rhs, const Locals& locals) {
        const auto left = expression(lhs, locals), right = expression(rhs, locals);
        if ((left.kind != Kind::Int && left.kind != Kind::Bool) || !(left == right))
            fail(lhs.offset, "equality operands must have matching int or bool types");
    };
    // Constructor slots currently have explicit int types. Bool literals can
    // initialize bool fields; integer bit patterns cannot become boolean storage.
    for (const auto& cls : program.classes) {
        Locals parameters;
        for (const auto& name : cls.constructor_parameters) parameters.emplace(name, scalar(false));
        for (const auto& field : cls.fields)
            if (field.initializer_expression)
                require(expression(*field.initializer_expression, parameters), scalar(field.scalar_type == ScalarType::Bool),
                        field.initializer_expression->offset, "class field initializer type");
    }
    std::function<void(const Function&, Locals)> function;
    function = [&](const Function& fn, Locals locals) {
        for (const auto& parameter : fn.parameters) locals.emplace(parameter.name, parameter_type(parameter));
        for (const auto& binding : fn.locals) {
            if (binding.initializer)
                require(expression(*binding.initializer, locals), local_type(binding), binding.offset, "local initializer type");
            for (const auto& arg : binding.constructor_arguments)
                require(expression(*arg, locals), scalar(false), arg->offset, "constructor parameter requires int");
            locals.emplace(binding.name, local_type(binding));
        }
        if (fn.scalar_while)
            require(locals.at(fn.scalar_while->counter), scalar(false), fn.offset, "loop counter requires int");
        std::function<void(const std::vector<Statement>&, Locals)> statements;
        statements = [&](const std::vector<Statement>& items, Locals scope) {
            for (const auto& item : items) {
                if (item.kind == Statement::Kind::ScopedLocal) {
                    const auto& binding = *item.branch_local;
                    require(expression(*binding.initializer, scope), local_type(binding), binding.offset, "branch local initializer");
                    scope.emplace(binding.name, local_type(binding));
                } else if (item.kind == Statement::Kind::Conditional) {
                    if (item.condition) require(expression(*item.condition, scope), scalar(true), item.offset, "condition requires bool");
                    else compare(*item.condition_lhs, *item.condition_rhs, scope);
                    statements(item.then_statements, scope); statements(item.else_statements, scope);
                } else if (item.kind == Statement::Kind::ArrayCommitFilled) {
                    require(expression(*item.array_commit_length, scope), scalar(false), item.offset, "array length requires int");
                    for (const auto& arg : item.array_commit_values)
                        require(expression(*arg, scope), scalar(false), arg->offset, "Array<int> fill requires int");
                } else if (!item.receiver.empty()) {
                    require(expression(*item.expression, scope), field_type(scope.at(item.receiver), item.field, item.offset),
                            item.offset, "field assignment type");
                } else if (item.expression) expression(*item.expression, scope);
            }
        };
        statements(fn.statements, locals); statements(fn.caught_throw_defers, locals);
        if (fn.caught_throw_value)
            require(expression(*fn.caught_throw_value, locals), scalar(false), fn.offset, "throw payload requires int");
        if (!fn.catch_binding.empty()) locals.emplace(fn.catch_binding, scalar(false));
        const Type result = fn.returns_void ? Type{Kind::Void, {}} : scalar(fn.return_type == ScalarType::Bool);
        for (const auto& guard : fn.early_returns) {
            if (guard.condition) require(expression(*guard.condition, locals), scalar(true), guard.offset, "guard requires bool");
            else compare(*guard.lhs, *guard.rhs, locals);
            require(expression(*guard.result, locals), result, guard.offset, "guarded return type");
        }
        if (fn.result) require(expression(*fn.result, locals), result, fn.offset, "return type");
        for (const auto& send : fn.actor_sends)
            require(expression(*send.payload, locals), scalar(false), send.offset, "actor message requires int");
        for (const auto& write : fn.actor_field_writes)
            require(expression(*write.expression, locals), scalar(false), write.offset, "actor field requires int");
    };
    for (const auto& fn : program.functions) {
        if (fn.name == "main" && fn.return_type == ScalarType::Bool) fail(fn.offset, "OS entrypoint requires int or void");
        function(fn, {});
    }
    for (const auto& actor : program.actors) {
        Locals fields;
        for (const auto& field : actor.fields) {
            require(expression(*field.initializer, fields), scalar(false), field.offset, "actor initializer requires int");
            fields.emplace(field.name, scalar(false));
        }
        function(actor.on_message, std::move(fields));
    }
}
}
