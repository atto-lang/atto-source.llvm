#include "oreslang/frontend/ModelGraph.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iomanip>
#include <functional>
#include <limits>
#include <memory>
#include <sstream>
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

std::uint32_t rotate_right(std::uint32_t value, unsigned bits) {
    return (value >> bits) | (value << (32U - bits));
}

const std::array<std::uint32_t, 64> sha256_k{{
    0x428a2f98U,0x71374491U,0xb5c0fbcfU,0xe9b5dba5U,
    0x3956c25bU,0x59f111f1U,0x923f82a4U,0xab1c5ed5U,
    0xd807aa98U,0x12835b01U,0x243185beU,0x550c7dc3U,
    0x72be5d74U,0x80deb1feU,0x9bdc06a7U,0xc19bf174U,
    0xe49b69c1U,0xefbe4786U,0x0fc19dc6U,0x240ca1ccU,
    0x2de92c6fU,0x4a7484aaU,0x5cb0a9dcU,0x76f988daU,
    0x983e5152U,0xa831c66dU,0xb00327c8U,0xbf597fc7U,
    0xc6e00bf3U,0xd5a79147U,0x06ca6351U,0x14292967U,
    0x27b70a85U,0x2e1b2138U,0x4d2c6dfcU,0x53380d13U,
    0x650a7354U,0x766a0abbU,0x81c2c92eU,0x92722c85U,
    0xa2bfe8a1U,0xa81a664bU,0xc24b8b70U,0xc76c51a3U,
    0xd192e819U,0xd6990624U,0xf40e3585U,0x106aa070U,
    0x19a4c116U,0x1e376c08U,0x2748774cU,0x34b0bcb5U,
    0x391c0cb3U,0x4ed8aa4aU,0x5b9cca4fU,0x682e6ff3U,
    0x748f82eeU,0x78a5636fU,0x84c87814U,0x8cc70208U,
    0x90befffaU,0xa4506cebU,0xbef9a3f7U,0xc67178f2U
}};

std::string canonical_constant(std::int64_t value) {
    return "i64:" + std::to_string(value);
}

std::string canonical_expression(const Expression& expr,
                                 const std::unordered_set<std::string>& parameters) {
    using Kind = Expression::Kind;
    switch (expr.kind) {
        case Kind::Integer:
            return canonical_constant(expr.integer);
        case Kind::Variable:
            if (!parameters.count(expr.name))
                model_fail("MODEL_EFFECTFUL_BODY",
                           "verified model op reads non-parameter '" + expr.name + "'");
            return "name:" + expr.name;
        case Kind::Negate:
            if (!expr.left)
                model_fail("MODEL_EFFECTFUL_BODY", "malformed unary model expression");
            return "unary:-(" + canonical_expression(*expr.left, parameters) + ")";
        case Kind::Add:
        case Kind::Subtract:
        case Kind::Multiply: {
            if (!expr.left || !expr.right)
                model_fail("MODEL_EFFECTFUL_BODY", "malformed binary model expression");
            const char op = expr.kind == Kind::Add ? '+' :
                            expr.kind == Kind::Subtract ? '-' : '*';
            return std::string("binary:") + op + "(" +
                canonical_expression(*expr.left, parameters) + "," +
                canonical_expression(*expr.right, parameters) + ")";
        }
        default:
            model_fail("MODEL_EFFECTFUL_BODY",
                       "model operation contains calls, ownership effects, fields or unsupported expressions");
    }
}

std::string operation_abi(std::size_t arity) {
    std::string out = "ores-model-op-v1(";
    for (std::size_t i = 0; i < arity; ++i) {
        if (i) out += ",";
        out += "int";
    }
    out += ")->int";
    return out;
}

std::string operation_impl_digest(const Function& fn) {
    if (!fn.result)
        model_fail("MODEL_INVALID_OP_SIGNATURE",
                   "operation '" + fn.name + "' has no return value");
    std::unordered_set<std::string> parameters;
    for (const auto& parameter : fn.parameters)
        parameters.insert(parameter.name);
    const std::string canonical =
        operation_abi(fn.parameters.size()) + "|return=" +
        canonical_expression(*fn.result, parameters);
    return model_sha256(canonical);
}

bool is_plain_scalar_operation(const Function& fn) {
    if (fn.returns_void || !fn.result || fn.parameters.empty() ||
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
            !parameter.class_name.empty() || !parameter.generic_type.empty())
            return false;
    }
    return true;
}

std::string qualified_model_name(const Program& program, const ModelDecl& model) {
    return program.module.empty() ? model.name : program.module + "." + model.name;
}

std::string static_arg_canonical(const ModelStaticArg& arg) {
    if (arg.kind == ModelStaticArg::Kind::Parameter)
        return "param:" + arg.parameter + ":int";
    return "lit:" + canonical_constant(arg.integer) + ":int";
}

std::string graph_digest(const Program& program,
                         const ModelDecl& model,
                         const std::unordered_map<std::string,
                             std::unordered_map<std::size_t, const Function*>>& functions) {
    std::string canonical = "ores-model-graph-v1|" +
        qualified_model_name(program, model) + "|";
    for (const auto& parameter : model.parameters) {
        canonical += "p:" + parameter.name + ":int:";
        canonical += parameter.has_default ?
            canonical_constant(parameter.default_value) : "required";
        canonical += "|";
    }
    for (std::size_t i = 0; i < model.operations.size(); ++i) {
        const auto& operation = model.operations[i];
        const std::size_t arity = operation.static_arguments.size() + 1;
        const auto family = functions.find(operation.name);
        if (family == functions.end() || !family->second.count(arity))
            model_fail("MODEL_UNKNOWN_OP",
                       "no native model operation '" + operation.name +
                       "' with arity " + std::to_string(arity));
        const Function& fn = *family->second.at(arity);
        canonical += "n" + std::to_string(i) + ":" + operation.name + ":" +
            operation_abi(arity) + ":" + operation_impl_digest(fn) + ":";
        for (const auto& arg : operation.static_arguments)
            canonical += static_arg_canonical(arg) + ",";
        canonical += "|";
    }
    return model_sha256(canonical);
}

std::string configured_digest(const ModelDecl& model,
                              const std::vector<std::int64_t>& configuration) {
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

std::unique_ptr<Expression> integer_expression(std::int64_t value,
                                               std::size_t offset) {
    auto result = std::make_unique<Expression>();
    result->kind = Expression::Kind::Integer;
    result->integer = value;
    result->offset = offset;
    return result;
}

std::int64_t constant_integer(const Expression& expr) {
    if (expr.kind == Expression::Kind::Integer) return expr.integer;
    if (expr.kind == Expression::Kind::Negate && expr.left &&
        expr.left->kind == Expression::Kind::Integer) {
        if (expr.left->integer == std::numeric_limits<std::int64_t>::min())
            model_fail("MODEL_CONST_OVERFLOW", "negated model constant overflows i64");
        return -expr.left->integer;
    }
    model_fail("MODEL_NONCONSTANT_CONFIG",
               "native model constructor arguments must be integer literals");
}

using ModelMap = std::unordered_map<std::string, ModelDecl*>;

void rewrite_model_expression(std::unique_ptr<Expression>& expr,
                              Program& program,
                              const ModelMap& models);

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
                              const ModelMap& models) {
    if (!expr) return;

    // The existing parser represents Foo(...).run(x) as ArrayCall(name=run)
    // over Call(name=Foo). Recognize only when Foo is a known model. Ordinary
    // array/class method syntax keeps its existing path.
    if (expr->kind == Expression::Kind::ArrayCall && expr->name == "run" &&
        expr->left && expr->left->kind == Expression::Kind::Call) {
        const auto found = models.find(expr->left->name);
        if (found != models.end()) {
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

            std::vector<std::int64_t> config;
            config.reserve(model.parameters.size());
            if (expr->left->arguments.size() > model.parameters.size())
                model_fail("MODEL_CONFIG_ARITY",
                           "too many native model configuration arguments");
            for (std::size_t i = 0; i < model.parameters.size(); ++i) {
                if (i < expr->left->arguments.size())
                    config.push_back(constant_integer(*expr->left->arguments[i]));
                else if (model.parameters[i].has_default)
                    config.push_back(model.parameters[i].default_value);
                else
                    model_fail("MODEL_CONFIG_ARITY",
                               "missing native model parameter '" +
                               model.parameters[i].name + "'");
            }

            auto current = std::move(expr->arguments.front());
            rewrite_model_expression(current, program, models);
            std::unordered_map<std::string, std::int64_t> bound;
            for (std::size_t i = 0; i < model.parameters.size(); ++i)
                bound.emplace(model.parameters[i].name, config[i]);

            for (const auto& operation : model.operations) {
                auto call = std::make_unique<Expression>();
                call->kind = Expression::Kind::Call;
                call->name = operation.name;
                call->offset = operation.offset;
                for (const auto& argument : operation.static_arguments) {
                    if (argument.kind == ModelStaticArg::Kind::Integer) {
                        call->arguments.push_back(
                            integer_expression(argument.integer, operation.offset));
                    } else {
                        const auto value = bound.find(argument.parameter);
                        if (value == bound.end())
                            model_fail("MODEL_UNKNOWN_FORMAL",
                                       "unknown model parameter '" +
                                       argument.parameter + "'");
                        call->arguments.push_back(
                            integer_expression(value->second, operation.offset));
                    }
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

    rewrite_model_expression(expr->left, program, models);
    rewrite_model_expression(expr->right, program, models);
    for (auto& argument : expr->arguments)
        rewrite_model_expression(argument, program, models);

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
    std::array<std::uint32_t, 8> state{{
        0x6a09e667U,0xbb67ae85U,0x3c6ef372U,0xa54ff53aU,
        0x510e527fU,0x9b05688cU,0x1f83d9abU,0x5be0cd19U
    }};

    std::vector<std::uint8_t> bytes(input.begin(), input.end());
    const std::uint64_t bit_length =
        static_cast<std::uint64_t>(bytes.size()) * 8ULL;
    bytes.push_back(0x80U);
    while ((bytes.size() % 64U) != 56U) bytes.push_back(0U);
    for (int shift = 56; shift >= 0; shift -= 8)
        bytes.push_back(static_cast<std::uint8_t>((bit_length >> shift) & 0xffU));

    for (std::size_t block = 0; block < bytes.size(); block += 64U) {
        std::array<std::uint32_t, 64> words{};
        for (std::size_t i = 0; i < 16; ++i) {
            const std::size_t at = block + i * 4U;
            words[i] =
                (static_cast<std::uint32_t>(bytes[at]) << 24U) |
                (static_cast<std::uint32_t>(bytes[at + 1]) << 16U) |
                (static_cast<std::uint32_t>(bytes[at + 2]) << 8U) |
                static_cast<std::uint32_t>(bytes[at + 3]);
        }
        for (std::size_t i = 16; i < 64; ++i) {
            const std::uint32_t s0 =
                rotate_right(words[i - 15], 7) ^
                rotate_right(words[i - 15], 18) ^
                (words[i - 15] >> 3U);
            const std::uint32_t s1 =
                rotate_right(words[i - 2], 17) ^
                rotate_right(words[i - 2], 19) ^
                (words[i - 2] >> 10U);
            words[i] = words[i - 16] + s0 + words[i - 7] + s1;
        }

        std::uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
        std::uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
        for (std::size_t i = 0; i < 64; ++i) {
            const std::uint32_t sum1 =
                rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
            const std::uint32_t choice = (e & f) ^ ((~e) & g);
            const std::uint32_t temp1 =
                h + sum1 + choice + sha256_k[i] + words[i];
            const std::uint32_t sum0 =
                rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
            const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temp2 = sum0 + majority;
            h = g; g = f; f = e; e = d + temp1;
            d = c; c = b; b = a; a = temp1 + temp2;
        }
        state[0] += a; state[1] += b; state[2] += c; state[3] += d;
        state[4] += e; state[5] += f; state[6] += g; state[7] += h;
    }

    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (const auto word : state) out << std::setw(8) << word;
    return out.str();
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
            if (parameter.has_default) default_seen = true;
            else if (default_seen)
                model_fail("MODEL_CONFIG_DEFAULT_ORDER",
                           "required model parameters cannot follow defaults");
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
            for (const auto& argument : operation.static_arguments)
                if (argument.kind == ModelStaticArg::Kind::Parameter &&
                    !parameter_names.count(argument.parameter))
                    model_fail("MODEL_UNKNOWN_FORMAL",
                               "unknown model parameter '" + argument.parameter + "'");
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
    for (const auto& fn : program.functions)
        if (models.count(fn.name))
            model_fail("MODEL_VALUE_NAME_COLLISION",
                       "model '" + fn.name + "' conflicts with function");

    detect_cycles(models);
    for (auto& model : program.models) {
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
            if (!is_plain_scalar_operation(fn))
                model_fail("MODEL_EFFECTFUL_BODY",
                           "operation '" + operation.name +
                           "' is not a verified pure scalar function");
            (void)operation_impl_digest(fn); // validates the return expression.
        }
        model.graph_digest = graph_digest(program, model, functions);
    }

    for (auto& function : program.functions)
        rewrite_function(function, program, models);
}

} // namespace oreslang
