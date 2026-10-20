#include "oreslang/frontend/TreeShaker.hpp"
#include "oreslang/frontend/CallableSelector.hpp"

#include <algorithm>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace oreslang {
namespace {

std::string qualify(const Program& program, const std::string& name) {
    return (program.module.empty() ? "__root__" : program.module) + "." + name;
}

void add_runtime(std::set<std::string>& features,
                 std::set<std::string>& operations,
                 const std::string& operation) {
    features.insert("runtime-intrinsics");
    operations.insert(operation);
}

void scan_expression(const Expression* expression,
                     std::set<std::string>& calls,
                     std::set<std::string>& features,
                     std::set<std::string>& operations) {
    if (!expression) return;
    using Kind = Expression::Kind;
    switch (expression->kind) {
        case Kind::Integer:
        case Kind::Variable:
            return;
        case Kind::Borrow:
        case Kind::BorrowMut:
        case Kind::Copy:
        case Kind::Take:
        case Kind::Share:
            features.insert("ownership");
            scan_expression(expression->left.get(), calls, features, operations);
            return;
        case Kind::Negate:
            scan_expression(expression->left.get(), calls, features, operations);
            return;
        case Kind::Add:
        case Kind::Subtract:
        case Kind::Multiply:
            scan_expression(expression->left.get(), calls, features, operations);
            scan_expression(expression->right.get(), calls, features, operations);
            return;
        case Kind::Field:
            scan_expression(expression->left.get(), calls, features, operations);
            return;
        case Kind::Call:
            calls.insert(native_function_slot(
                expression->name, expression->arguments.size()));
            for (const auto& argument : expression->arguments)
                scan_expression(argument.get(), calls, features, operations);
            return;
        case Kind::ArrayCall:
            add_runtime(features, operations, "native-int-array-region");
            scan_expression(expression->left.get(), calls, features, operations);
            for (const auto& argument : expression->arguments)
                scan_expression(argument.get(), calls, features, operations);
            return;
    }
    throw std::runtime_error("tree shaker encountered unknown expression kind");
}

void scan_local(const LocalBinding& local,
                std::set<std::string>& calls,
                std::set<std::string>& features,
                std::set<std::string>& operations) {
    scan_expression(local.initializer.get(), calls, features, operations);
    for (const auto& argument : local.constructor_arguments)
        scan_expression(argument.get(), calls, features, operations);
    if (local.heap_owned) {
        features.insert("ownership");
        add_runtime(features, operations, "native-owned-i64-region");
    }
    if (local.int_array) {
        features.insert("ownership");
        add_runtime(features, operations, "native-int-array-region");
    }
    if (local.new_class || !local.class_name.empty()) features.insert("classes");
}

void scan_statement(const Statement& statement,
                    std::set<std::string>& calls,
                    std::set<std::string>& features,
                    std::set<std::string>& operations) {
    if (statement.branch_local)
        scan_local(*statement.branch_local, calls, features, operations);
    scan_expression(statement.expression.get(), calls, features, operations);
    scan_expression(statement.array_commit_length.get(), calls, features, operations);
    for (const auto& value : statement.array_commit_values)
        scan_expression(value.get(), calls, features, operations);
    scan_expression(statement.condition_lhs.get(), calls, features, operations);
    scan_expression(statement.condition_rhs.get(), calls, features, operations);
    for (const auto& nested : statement.then_statements)
        scan_statement(nested, calls, features, operations);
    for (const auto& nested : statement.else_statements)
        scan_statement(nested, calls, features, operations);
    if (statement.kind == Statement::Kind::ArrayCommitFilled)
        add_runtime(features, operations, "native-int-array-region");
}

void scan_function(const Function& function,
                   std::set<std::string>& calls,
                   std::set<std::string>& features,
                   std::set<std::string>& operations) {
    if (function.scalar_while) {
        features.insert("suspension");
        add_runtime(features, operations, "cooperative-loop");
    }
    for (const auto& local : function.locals)
        scan_local(local, calls, features, operations);
    for (const auto& statement : function.statements)
        scan_statement(statement, calls, features, operations);
    for (const auto& guarded : function.early_returns) {
        scan_expression(guarded.lhs.get(), calls, features, operations);
        scan_expression(guarded.rhs.get(), calls, features, operations);
        scan_expression(guarded.result.get(), calls, features, operations);
    }
    if (!function.actor_spawns.empty() || !function.actor_sends.empty() ||
        !function.actor_stops.empty() || !function.actor_field_writes.empty()) {
        features.insert("actors");
        add_runtime(features, operations, "actor-runtime");
    }
    for (const auto& send : function.actor_sends)
        scan_expression(send.payload.get(), calls, features, operations);
    for (const auto& write : function.actor_field_writes)
        scan_expression(write.expression.get(), calls, features, operations);
    if (function.caught_throw_value) {
        features.insert("exceptions");
        scan_expression(function.caught_throw_value.get(), calls, features, operations);
    }
    for (const auto& statement : function.caught_throw_defers)
        scan_statement(statement, calls, features, operations);
    scan_expression(function.result.get(), calls, features, operations);
}

std::vector<std::string> sorted(const std::set<std::string>& values) {
    return std::vector<std::string>(values.begin(), values.end());
}

void append_quoted(std::ostringstream& out, const std::string& value) {
    out << '"';
    static const char* hex = "0123456789abcdef";
    for (const unsigned char ch : value) {
        switch (ch) {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\b': out << "\\b"; break;
            case '\f': out << "\\f"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (ch < 0x20) {
                    out << "\\u00" << hex[(ch >> 4) & 0x0f] << hex[ch & 0x0f];
                } else {
                    out << static_cast<char>(ch);
                }
        }
    }
    out << '"';
}

void append_array(std::ostringstream& out,
                  const char* key,
                  const std::vector<std::string>& values) {
    append_quoted(out, key);
    out << ":[";
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i) out << ',';
        append_quoted(out, values[i]);
    }
    out << ']';
}

} // namespace

std::string NativeReachabilityManifest::to_canonical_json() const {
    std::ostringstream out;
    out << '{';
    append_quoted(out, "schema");
    out << ':';
    append_quoted(out, "oreslang.reachability/1");
    out << ',';
    append_array(out, "entryPoints", entry_points);
    out << ",\"preservePublicApi\":" << (preserve_public_api ? "true" : "false") << ',';
    append_array(out, "retainedSymbols", retained_symbols);
    out << ',';
    append_array(out, "removedSymbols", removed_symbols);
    out << ',';
    append_array(out, "externalBindings", external_bindings);
    out << ',';
    append_array(out, "languageFeatures", language_features);
    out << ',';
    append_array(out, "runtimeOperations", runtime_operations);
    out << '}';
    return out.str();
}

NativeReachabilityManifest tree_shake_program(Program& program, TreeShakeMode mode) {
    std::unordered_map<std::string, Function*> functions;
    for (auto& function : program.functions) {
        const std::string slot =
            native_function_slot(function.name, function.parameters.size());
        if (!functions.emplace(slot, &function).second) {
            throw std::runtime_error(
                "native tree shaking found duplicate callable slot '" + slot + "'");
        }
    }

    std::set<std::string> roots;
    NativeReachabilityManifest manifest;
    manifest.preserve_public_api = mode == TreeShakeMode::Library;

    if (mode == TreeShakeMode::Executable) {
        const std::string main_slot = native_function_slot("main", 0);
        const auto main = functions.find(main_slot);
        if (main == functions.end())
            throw std::runtime_error("native executable tree shaking requires zero-arity main");
        roots.insert(main_slot);
        manifest.entry_points = {"main"};
    } else {
        for (const auto& function : program.functions) {
            if (function.exported) {
                roots.insert(native_function_slot(
                    function.name, function.parameters.size()));
            }
        }
    }

    std::set<std::string> retained;
    std::set<std::string> features;
    std::set<std::string> operations;
    std::set<std::string> conservative_calls;

    // Native v1 deliberately keeps all classes and actors. Their dependencies
    // therefore root top-level functions too; pruning them first would be unsound.
    for (const auto& klass : program.classes) {
        features.insert("classes");
        manifest.retained_symbols.push_back(qualify(program, klass.name));
        for (const auto& field : klass.fields) {
            scan_expression(field.initializer_expression.get(),
                            conservative_calls, features, operations);
            if (field.int_array) {
                features.insert("ownership");
                add_runtime(features, operations, "native-int-array-region");
            }
        }
    }
    for (const auto& actor : program.actors) {
        features.insert("actors");
        add_runtime(features, operations, "actor-runtime");
        manifest.retained_symbols.push_back(qualify(program, actor.name));
        for (const auto& field : actor.fields)
            scan_local(field, conservative_calls, features, operations);
        scan_function(actor.on_message, conservative_calls, features, operations);
    }
    if (!program.cooperative_loops.empty()) {
        features.insert("suspension");
        add_runtime(features, operations, "cooperative-loop");
    }
    roots.insert(conservative_calls.begin(), conservative_calls.end());

    std::vector<std::string> work(roots.begin(), roots.end());
    for (std::size_t index = 0; index < work.size(); ++index) {
        const std::string& slot = work[index];
        if (!retained.insert(slot).second) continue;
        const auto found = functions.find(slot);
        if (found == functions.end())
            throw std::runtime_error("native tree-shake call/root does not resolve: " + slot);

        std::set<std::string> calls;
        scan_function(*found->second, calls, features, operations);
        for (const auto& callee_slot : calls) {
            const auto target = functions.find(callee_slot);
            if (target == functions.end())
                throw std::runtime_error(
                    "native tree-shake call does not resolve: " + callee_slot);
            if (!retained.count(callee_slot)) work.push_back(callee_slot);
        }
    }

    std::set<std::string> removed;
    for (const auto& function : program.functions) {
        const std::string slot =
            native_function_slot(function.name, function.parameters.size());
        const std::string symbol = qualify(program, slot);
        if (retained.count(slot)) manifest.retained_symbols.push_back(symbol);
        else removed.insert(symbol);
    }

    program.functions.erase(
        std::remove_if(program.functions.begin(), program.functions.end(),
            [&](const Function& function) {
                return !retained.count(native_function_slot(
                    function.name, function.parameters.size()));
            }),
        program.functions.end());

    std::sort(manifest.retained_symbols.begin(), manifest.retained_symbols.end());
    manifest.retained_symbols.erase(
        std::unique(manifest.retained_symbols.begin(), manifest.retained_symbols.end()),
        manifest.retained_symbols.end());
    manifest.removed_symbols = sorted(removed);
    manifest.language_features = sorted(features);
    manifest.runtime_operations = sorted(operations);
    return manifest;
}

} // namespace oreslang
