#include "oreslang/frontend/ModelGraph.hpp"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/SHA256.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace oreslang {
namespace {

constexpr std::size_t kMaxModels = 1024;
constexpr std::size_t kMaxModelParameters = 64;
constexpr std::size_t kMaxModelNodes = 4096;
constexpr std::size_t kMaxStaticArgsPerNode = 32;
constexpr std::size_t kMaxTotalStaticArgs = 16384;
constexpr std::size_t kMaxIdentifier = 128;
// Each operation becomes one ordinary native call around the model input.
// Bound the generated chain before materializing it, including nested runs.
constexpr std::size_t kMaxLoweredOperationsPerPath = 63;

[[noreturn]] void model_fail(std::string_view code, const std::string& detail) {
    throw std::runtime_error(std::string(code) + ": " + detail);
}

bool ascii_letter(char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
}

void validate_identifier(const std::string& name, std::string_view label) {
    if (name.empty() || name.size() > kMaxIdentifier || !ascii_letter(name.front()))
        model_fail("MODEL_INVALID_IDENTIFIER",
                   std::string(label) + " identifier is invalid or too long");
    for (std::size_t i = 1; i < name.size(); ++i) {
        const char ch = name[i];
        if (!(ascii_letter(ch) || (ch >= '0' && ch <= '9') || ch == '_'))
            model_fail("MODEL_INVALID_IDENTIFIER",
                       std::string(label) + " identifier '" + name +
                           "' is not canonical ASCII");
    }
}

bool is_model_scalar(ScalarType type) {
    return type == ScalarType::I64 || type == ScalarType::F64;
}

ScalarType model_scalar_type(const ModelScalar& value) {
    return std::holds_alternative<double>(value) ? ScalarType::F64 : ScalarType::I64;
}

const char* model_scalar_name(ScalarType type) {
    if (!is_model_scalar(type))
        model_fail("MODEL_INVALID_OP_SIGNATURE", "model scalar must be int or float");
    return type == ScalarType::F64 ? "float" : "int";
}

std::string canonical_constant(const ModelScalar& value) {
    if (const auto* integer = std::get_if<std::int64_t>(&value))
        return "i64:" + std::to_string(*integer);

    const double number = std::get<double>(value);
    if (!std::isfinite(number))
        model_fail("MODEL_NONFINITE_CONST", "model float literal must be finite");
    std::uint64_t bits;
    static_assert(sizeof(bits) == sizeof(number));
    std::memcpy(&bits, &number, sizeof(bits));
    return "f64bits:" + llvm::utohexstr(bits, /*LowerCase=*/true, /*Width=*/16);
}

std::string canonical_expression(
        const Expression& expr,
        const std::unordered_map<std::string, std::size_t>& parameter_indexes) {
    using Kind = Expression::Kind;
    switch (expr.kind) {
        case Kind::Integer:
            return canonical_constant(expr.integer);
        case Kind::Floating:
            return canonical_constant(expr.floating);
        case Kind::Variable: {
            const auto found = parameter_indexes.find(expr.name);
            if (found == parameter_indexes.end())
                model_fail("MODEL_EFFECTFUL_BODY",
                           "verified model op reads non-parameter '" + expr.name + "'");
            return "arg:" + std::to_string(found->second);
        }
        case Kind::Negate:
            if (!expr.left)
                model_fail("MODEL_EFFECTFUL_BODY", "malformed unary model expression");
            return "unary:-(" + canonical_expression(*expr.left, parameter_indexes) + ")";
        case Kind::Add:
        case Kind::Subtract:
        case Kind::Multiply:
        case Kind::Divide: {
            if (!expr.left || !expr.right)
                model_fail("MODEL_EFFECTFUL_BODY", "malformed binary model expression");
            const char op = expr.kind == Kind::Add ? '+' :
                            expr.kind == Kind::Subtract ? '-' :
                            expr.kind == Kind::Multiply ? '*' : '/';
            return std::string("binary:") + op + "(" +
                canonical_expression(*expr.left, parameter_indexes) + "," +
                canonical_expression(*expr.right, parameter_indexes) + ")";
        }
        default:
            model_fail("MODEL_EFFECTFUL_BODY",
                       "model operation contains calls, ownership effects, fields or unsupported expressions");
    }
}

std::string operation_abi(const Function& fn) {
    std::string out = "ores-model-op-v1(";
    for (std::size_t i = 0; i < fn.parameters.size(); ++i) {
        if (i) out += ",";
        out += model_scalar_name(fn.parameters[i].scalar_type);
    }
    out += ")->";
    out += model_scalar_name(fn.return_type);
    return out;
}

std::string operation_impl_digest(const Function& fn) {
    if (!fn.result)
        model_fail("MODEL_INVALID_OP_SIGNATURE",
                   "operation '" + fn.name + "' has no return value");
    std::unordered_map<std::string, std::size_t> parameter_indexes;
    for (std::size_t i = 0; i < fn.parameters.size(); ++i) {
        if (!parameter_indexes.emplace(fn.parameters[i].name, i).second)
            model_fail("MODEL_INVALID_OP_SIGNATURE",
                       "operation '" + fn.name + "' has duplicate parameter '" +
                       fn.parameters[i].name + "'");
    }
    const std::string canonical =
        operation_abi(fn) + "|return=" +
        canonical_expression(*fn.result, parameter_indexes);
    return model_sha256(canonical);
}

bool is_plain_scalar_operation(const Function& fn) {
    if (fn.returns_void || !is_model_scalar(fn.return_type) ||
        !fn.return_class.empty() || !fn.return_sum.empty() ||
        !fn.pending_return_type_alias.empty() || !fn.result || fn.parameters.empty() ||
        !fn.method_owner.empty() || fn.async_source_i64 || fn.foreign_import ||
        !fn.c_export_u32_symbol.empty() || !fn.source_tasks.empty() ||
        !fn.future_pipelines.empty() || !fn.actor_task_await_name.empty() ||
        !fn.ready_await_actor.empty() || !fn.done_await_actor.empty() ||
        !fn.ready_future_name.empty() || !fn.ready_future_actor.empty() ||
        !fn.generic_parameters.empty() || fn.scalar_while ||
        !fn.locals.empty() || !fn.statements.empty() ||
        !fn.early_returns.empty() || !fn.actor_spawns.empty() ||
        !fn.actor_sends.empty() || !fn.actor_stops.empty() ||
        !fn.actor_actions.empty() || !fn.actor_field_writes.empty() ||
        fn.caught_throw_value || !fn.caught_throw_defers.empty() ||
        fn.has_declared_effects)
        return false;
    for (const auto& parameter : fn.parameters) {
        if (parameter.mode != Parameter::Mode::Value ||
            !is_model_scalar(parameter.scalar_type) ||
            !parameter.class_name.empty() || !parameter.generic_type.empty() ||
            !parameter.pending_type_alias.empty() || !parameter.sum_type.empty())
            return false;
    }
    return true;
}

std::string qualified_model_name(const Program& program, const ModelDecl& model) {
    return program.module.empty() ? model.name : program.module + "." + model.name;
}

using ModelParameterTypes = std::unordered_map<std::string, ScalarType>;

ScalarType static_arg_type(const ModelStaticArg& arg, const ModelParameterTypes& types) {
    if (const auto* name = std::get_if<std::string>(&arg.value))
        return types.at(*name);
    return model_scalar_type(std::get<ModelScalar>(arg.value));
}

std::string static_arg_canonical(const ModelStaticArg& arg,
                                 const ModelParameterTypes& types) {
    const char* type = model_scalar_name(static_arg_type(arg, types));
    if (const auto* name = std::get_if<std::string>(&arg.value))
        return "param:" + *name + ":" + type;
    return "lit:" + canonical_constant(std::get<ModelScalar>(arg.value)) + ":" + type;
}

std::string graph_digest(const Program& program,
                         const ModelDecl& model,
                         const ModelParameterTypes& types,
                         const std::vector<const Function*>& functions) {
    std::string canonical = "ores-model-graph-v1|" +
        qualified_model_name(program, model) + "|";
    for (const auto& parameter : model.parameters) {
        canonical += "p:" + parameter.name + ":" + model_scalar_name(parameter.type) + ":";
        canonical += parameter.default_value ?
            canonical_constant(*parameter.default_value) : "required";
        canonical += "|";
    }
    for (std::size_t i = 0; i < model.operations.size(); ++i) {
        const auto& operation = model.operations[i];
        const Function& fn = *functions.at(i);
        canonical += "n" + std::to_string(i) + ":" + operation.name + ":" +
            operation_abi(fn) + ":" + operation_impl_digest(fn) + ":";
        for (const auto& arg : operation.static_arguments)
            canonical += static_arg_canonical(arg, types) + ",";
        canonical += "|";
    }
    return model_sha256(canonical);
}

std::string configured_digest(const ModelDecl& model,
                              const std::vector<ModelScalar>& configuration) {
    if (configuration.size() != model.parameters.size())
        model_fail("MODEL_CONFIG_SHAPE",
                   "configured model has wrong parameter cardinality");
    std::string canonical =
        "ores-model-configured-v1|" + model.graph_digest + "|";
    for (std::size_t i = 0; i < model.parameters.size(); ++i)
        canonical += model.parameters[i].name + "=" +
            canonical_constant(configuration[i]) + "|";
    return model_sha256(canonical);
}

std::unique_ptr<Expression> scalar_expression(const ModelScalar& value,
                                              std::size_t offset) {
    auto result = std::make_unique<Expression>();
    result->offset = offset;
    if (const auto* integer = std::get_if<std::int64_t>(&value)) {
        result->kind = Expression::Kind::Integer;
        result->integer = *integer;
    } else {
        result->kind = Expression::Kind::Floating;
        result->floating = std::get<double>(value);
    }
    return result;
}

ModelScalar constant_scalar(const Expression& expr) {
    if (expr.kind == Expression::Kind::Integer) return expr.integer;
    if (expr.kind == Expression::Kind::Floating) return expr.floating;
    if (expr.kind == Expression::Kind::Negate && expr.left) {
        if (expr.left->kind == Expression::Kind::Floating) return -expr.left->floating;
        if (expr.left->kind == Expression::Kind::Integer) {
            if (expr.left->integer == std::numeric_limits<std::int64_t>::min())
                model_fail("MODEL_CONST_OVERFLOW", "negated model constant overflows i64");
            return -expr.left->integer;
        }
    }
    model_fail("MODEL_NONCONSTANT_CONFIG",
               "native model constructor arguments must be numeric literals");
}

using ModelMap = std::unordered_map<std::string, ModelDecl*>;

void rewrite_model_expression(std::unique_ptr<Expression>& expr,
                              Program& program,
                              const ModelMap& models,
                              std::size_t remaining_operations = kMaxLoweredOperationsPerPath);

void rewrite_statement(Statement& stmt, Program& program,
                       const ModelMap& models) {
    if (stmt.branch_local) {
        rewrite_model_expression(stmt.branch_local->initializer, program, models);
        for (auto& argument : stmt.branch_local->constructor_arguments)
            rewrite_model_expression(argument, program, models);
    }
    rewrite_model_expression(stmt.expression, program, models);
    rewrite_model_expression(stmt.array_commit_length, program, models);
    for (auto& item : stmt.array_commit_values)
        rewrite_model_expression(item, program, models);
    rewrite_model_expression(stmt.loop_step, program, models);
    rewrite_model_expression(stmt.condition, program, models);
    rewrite_model_expression(stmt.condition_lhs, program, models);
    rewrite_model_expression(stmt.condition_rhs, program, models);
    for (auto& nested : stmt.then_statements)
        rewrite_statement(nested, program, models);
    for (auto& nested : stmt.else_statements)
        rewrite_statement(nested, program, models);
}

void rewrite_function(Function& fn, Program& program, const ModelMap& models) {
    for (auto& local : fn.locals) {
        rewrite_model_expression(local.initializer, program, models);
        for (auto& argument : local.constructor_arguments)
            rewrite_model_expression(argument, program, models);
    }
    for (auto& stmt : fn.statements)
        rewrite_statement(stmt, program, models);
    for (auto& guard : fn.early_returns) {
        rewrite_model_expression(guard.condition, program, models);
        rewrite_model_expression(guard.lhs, program, models);
        rewrite_model_expression(guard.rhs, program, models);
        rewrite_model_expression(guard.result, program, models);
    }
    rewrite_model_expression(fn.caught_throw_value, program, models);
    for (auto& stmt : fn.caught_throw_defers)
        rewrite_statement(stmt, program, models);
    rewrite_model_expression(fn.result, program, models);
}

void rewrite_model_expression(std::unique_ptr<Expression>& expr,
                              Program& program,
                              const ModelMap& models,
                              std::size_t remaining_operations) {
    if (!expr) return;

    // The existing parser represents Foo(...).run(x) as ArrayCall(name=run)
    // over Call(name=Foo). Recognize only when Foo is a known model. Ordinary
    // array/class method syntax keeps its existing path.
    if (expr->kind == Expression::Kind::ArrayCall && expr->name == "run" &&
        expr->left && expr->left->kind == Expression::Kind::Call) {
        const auto found = models.find(expr->left->name);
        if (found != models.end() && !expr->left->pipeline_stage) {
            ModelDecl& model = *found->second;
            if (expr->left->type_arguments_present)
                model_fail("MODEL_CONFIG_TYPE_ARGUMENTS",
                           "native model constructors do not accept generic arguments");
            if (std::any_of(expr->left->argument_names.begin(),
                            expr->left->argument_names.end(),
                            [](const std::string& label) { return !label.empty(); }))
                model_fail("MODEL_CONFIG_NAMED_UNSUPPORTED",
                           "native executable-v1 model configuration is positional");
            if (expr->arguments.size() != 1)
                model_fail("MODEL_RUN_ARITY",
                           "native model.run expects exactly one input");
            if (std::any_of(expr->argument_names.begin(), expr->argument_names.end(),
                            [](const std::string& label) { return !label.empty(); }))
                model_fail("MODEL_RUN_NAMED_UNSUPPORTED",
                           "native model.run requires a positional input");

            std::vector<ModelScalar> config;
            config.reserve(model.parameters.size());
            if (expr->left->arguments.size() > model.parameters.size())
                model_fail("MODEL_CONFIG_ARITY",
                           "too many native model configuration arguments");
            for (std::size_t i = 0; i < model.parameters.size(); ++i) {
                const auto& parameter = model.parameters[i];
                if (i < expr->left->arguments.size())
                    config.push_back(constant_scalar(*expr->left->arguments[i]));
                else if (parameter.default_value)
                    config.push_back(*parameter.default_value);
                else
                    model_fail("MODEL_CONFIG_ARITY",
                               "missing native model parameter '" + parameter.name + "'");
                if (model_scalar_type(config.back()) != parameter.type)
                    model_fail("MODEL_CONFIG_TYPE_MISMATCH",
                               "native model parameter '" + parameter.name + "' requires " +
                               model_scalar_name(parameter.type));
            }

            if (model.operations.size() > remaining_operations)
                model_fail("MODEL_LOWERING_DEPTH",
                           "native model runs exceed 63 lowered operations on one expression path");
            auto current = std::move(expr->arguments.front());
            rewrite_model_expression(current, program, models,
                                     remaining_operations - model.operations.size());
            std::unordered_map<std::string, ModelScalar> bound;
            for (std::size_t i = 0; i < model.parameters.size(); ++i)
                bound.emplace(model.parameters[i].name, config[i]);

            for (const auto& operation : model.operations) {
                auto call = std::make_unique<Expression>();
                call->kind = Expression::Kind::Call;
                call->name = operation.name;
                call->offset = operation.offset;
                for (const auto& argument : operation.static_arguments) {
                    const auto* parameter = std::get_if<std::string>(&argument.value);
                    const ModelScalar& value = parameter ? bound.at(*parameter) :
                        std::get<ModelScalar>(argument.value);
                    call->arguments.push_back(scalar_expression(value, operation.offset));
                }
                call->arguments.push_back(std::move(current));
                current = std::move(call);
            }

            program.model_runs.push_back(ModelRunReceipt{
                qualified_model_name(program, model),
                model.graph_digest,
                configured_digest(model, config),
                config
            });
            expr = std::move(current);
            return;
        }
    }

    rewrite_model_expression(expr->left, program, models, remaining_operations);
    rewrite_model_expression(expr->right, program, models, remaining_operations);
    rewrite_model_expression(expr->condition, program, models, remaining_operations);
    rewrite_model_expression(expr->condition_lhs, program, models, remaining_operations);
    rewrite_model_expression(expr->condition_rhs, program, models, remaining_operations);
    rewrite_model_expression(expr->when_true, program, models, remaining_operations);
    rewrite_model_expression(expr->when_false, program, models, remaining_operations);
    for (auto& argument : expr->arguments)
        rewrite_model_expression(argument, program, models, remaining_operations);

    if (expr->kind == Expression::Kind::Call && models.count(expr->name))
        model_fail("MODEL_VALUE_STORAGE_UNSUPPORTED",
                   "native v1 model values must be immediately consumed by .run(input)");
}

void detect_cycles(const ModelMap& models) {
    std::unordered_set<std::string> visited;
    std::unordered_set<std::string> active;
    std::function<void(const std::string&)> visit =
        [&](const std::string& name) {
            if (visited.count(name)) return;
            if (!active.insert(name).second)
                model_fail("MODEL_RECURSIVE_COMPOSITION",
                           "recursive native model composition involving '" + name + "'");
            const ModelDecl& model = *models.at(name);
            for (const auto& operation : model.operations)
                if (models.count(operation.name)) visit(operation.name);
            active.erase(name);
            visited.insert(name);
        };
    for (const auto& item : models) visit(item.first);
}

} // namespace

std::string model_sha256(std::string_view input) {
    llvm::SHA256 sha;
    sha.update(llvm::StringRef(input.data(), input.size()));
    constexpr std::string_view hex = "0123456789abcdef";
    std::string digest;
    digest.reserve(64);
    for (const auto byte : sha.final()) {
        digest.push_back(hex[byte >> 4]);
        digest.push_back(hex[byte & 0xf]);
    }
    return digest;
}

void admit_and_lower_models(Program& program) {
    if (program.models.empty()) return;
    if (program.models.size() > kMaxModels)
        model_fail("MODEL_BUDGET", "native model count exceeds 1024");

    ModelMap models;
    for (auto& model : program.models) {
        validate_identifier(model.name, "model");
        if (!models.emplace(model.name, &model).second)
            model_fail("MODEL_DUPLICATE", "duplicate model '" + model.name + "'");
        if (model.parameters.size() > kMaxModelParameters)
            model_fail("MODEL_CONFIG_BUDGET",
                       "native model configuration exceeds 64 parameters");
        if (model.operations.empty())
            model_fail("MODEL_EMPTY_BODY", "native model requires at least one operation");
        if (model.operations.size() > kMaxModelNodes)
            model_fail("MODEL_NODE_BUDGET", "native model exceeds 4096 operations");

        bool default_seen = false;
        std::unordered_set<std::string> parameter_names;
        std::size_t static_total = 0;
        for (const auto& parameter : model.parameters) {
            validate_identifier(parameter.name, "model parameter");
            if (!parameter_names.insert(parameter.name).second)
                model_fail("MODEL_DUPLICATE_PARAMETER",
                           "duplicate model parameter '" + parameter.name + "'");
            if (!is_model_scalar(parameter.type))
                model_fail("MODEL_CONFIG_TYPE_MISMATCH", "model configuration must be int or float");
            if (parameter.default_value) {
                if (model_scalar_type(*parameter.default_value) != parameter.type)
                    model_fail("MODEL_CONFIG_TYPE_MISMATCH",
                               "default for model parameter '" + parameter.name + "' requires " +
                               model_scalar_name(parameter.type));
                default_seen = true;
            } else if (default_seen) {
                model_fail("MODEL_CONFIG_DEFAULT_ORDER",
                           "required model parameters cannot follow defaults");
            }
        }
        for (const auto& operation : model.operations) {
            validate_identifier(operation.name, "model operation");
            if (operation.static_arguments.size() > kMaxStaticArgsPerNode)
                model_fail("MODEL_STATIC_ARG_BUDGET",
                           "native model operation exceeds 32 static arguments");
            if (static_total > kMaxTotalStaticArgs -
                    std::min(kMaxTotalStaticArgs, operation.static_arguments.size()))
                model_fail("MODEL_STATIC_ARG_BUDGET",
                           "native model exceeds static argument budget");
            static_total += operation.static_arguments.size();
            for (const auto& argument : operation.static_arguments) {
                const auto* parameter = std::get_if<std::string>(&argument.value);
                if (parameter && !parameter_names.count(*parameter))
                    model_fail("MODEL_UNKNOWN_FORMAL",
                               "unknown model parameter '" + *parameter + "'");
            }
        }
    }

    std::unordered_map<std::string,
        std::unordered_map<std::size_t, const Function*>> functions;
    for (const auto& fn : program.functions) {
        auto& family = functions[fn.name];
        if (!family.emplace(fn.parameters.size(), &fn).second)
            model_fail("MODEL_AMBIGUOUS_OP",
                       "function '" + fn.name + "' has duplicate native arity");
    }

    for (const auto& cls : program.classes)
        if (models.count(cls.name))
            model_fail("MODEL_TYPE_NAME_COLLISION",
                       "model '" + cls.name + "' conflicts with class");
    for (const auto& actor : program.actors)
        if (models.count(actor.name))
            model_fail("MODEL_TYPE_NAME_COLLISION",
                       "model '" + actor.name + "' conflicts with actor");
    for (const auto& type : program.type_only_declarations)
        if (models.count(type.name))
            model_fail("MODEL_TYPE_NAME_COLLISION",
                       "model '" + type.name + "' conflicts with a type-only declaration");
    for (const auto& type : program.sum_types)
        if (models.count(type.name))
            model_fail("MODEL_TYPE_NAME_COLLISION",
                       "model '" + type.name + "' conflicts with a sum type");
    for (const auto& fn : program.functions)
        if (models.count(fn.name))
            model_fail("MODEL_VALUE_NAME_COLLISION",
                       "model '" + fn.name + "' conflicts with function");

    detect_cycles(models);
    for (auto& model : program.models) {
        ModelParameterTypes types;
        bool portable_i64 = true;
        for (const auto& parameter : model.parameters) {
            types.emplace(parameter.name, parameter.type);
            portable_i64 &= parameter.type == ScalarType::I64;
        }

        std::vector<const Function*> resolved;
        for (const auto& operation : model.operations) {
            if (models.count(operation.name))
                model_fail("MODEL_COMPOSITION_NOT_ENABLED",
                           "native executable-v1 model composition is not enabled");
            const std::size_t arity = operation.static_arguments.size() + 1;
            const auto family = functions.find(operation.name);
            if (family == functions.end() || !family->second.count(arity))
                model_fail("MODEL_UNKNOWN_OP",
                           "no native model operation '" + operation.name +
                           "' with arity " + std::to_string(arity));
            const Function& fn = *family->second.at(arity);
            if (fn.source_routine || fn.source_signature_has_scalar_alias)
                model_fail("MODEL_INVALID_OP_SIGNATURE",
                           "operation '" + operation.name +
                           "' must use fnc with a direct numeric signature");
            if (!is_plain_scalar_operation(fn))
                model_fail("MODEL_EFFECTFUL_BODY",
                           "operation '" + operation.name +
                           "' is not a verified pure scalar function");

            for (std::size_t i = 0; i < operation.static_arguments.size(); ++i)
                if (static_arg_type(operation.static_arguments[i], types) != fn.parameters[i].scalar_type)
                    model_fail("MODEL_CONFIG_TYPE_MISMATCH",
                               "static argument of '" + operation.name + "' requires " +
                               model_scalar_name(fn.parameters[i].scalar_type));
            if (!resolved.empty() && resolved.back()->return_type != fn.parameters.back().scalar_type)
                model_fail("MODEL_PORT_TYPE_MISMATCH",
                           "model output from '" + resolved.back()->name +
                           "' does not match the input to '" + operation.name + "'");
            portable_i64 &= !fn.has_f64_signature();
            resolved.push_back(&fn);
        }
        model.graph_digest = graph_digest(program, model, types, resolved);
        model.portable_profile = portable_i64 ? "model-core-i64-v1" : "";
    }

    for (auto& function : program.functions)
        rewrite_function(function, program, models);
}

} // namespace oreslang
