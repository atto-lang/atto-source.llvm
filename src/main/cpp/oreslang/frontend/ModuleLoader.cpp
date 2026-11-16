#include "oreslang/frontend/ModuleLoader.hpp"
#include "oreslang/frontend/CoreLibrarySources.hpp"
#include "oreslang/frontend/ImportPathVariables.hpp"
#include "oreslang/frontend/ProjectRootImport.hpp"
#include <cstdlib>
#include "oreslang/types/NativeSumTypes.hpp"
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace oreslang {
namespace {
namespace fs = std::filesystem;
constexpr std::size_t max_bytes = 8u * 1024u * 1024u;
constexpr std::size_t max_units = 128, max_depth = 32;
struct Unit {
    fs::path path;
    SourceIdentity identity;
    Program program;
    std::string prefix;
    bool compiler_owned_library = false;
    frontend::CompilerLibraryTier compiler_library_tier =
        frontend::CompilerLibraryTier::Core;
    std::uint32_t admitted_core_provider_bits = 0;
    frontend::CoreClosureRequirements core_closure{};
    bool core_closure_complete = false;
    std::unordered_map<std::string, std::size_t> dependencies;
    std::unordered_map<std::string, std::pair<std::size_t, std::string>> class_imports;
    // local name -> (unit index, pub function name) from `import fnc`.
    std::unordered_map<std::string, std::pair<std::size_t, std::string>> function_imports;
    // Declaration identities stay immutable while ASTs are moved/qualified.
    std::unordered_map<std::string, std::unordered_map<std::size_t, bool>> functions;
    std::unordered_set<std::string> classes;
    std::unordered_map<std::string, bool> class_exports;
    std::unordered_map<std::string, bool> constructor_exports;
};
class Loader {
    std::vector<std::unique_ptr<Unit>> units;
    std::size_t bytes = 0, read_bytes = 0, edges = 0;
    std::unordered_map<std::string, std::size_t> paths;
    // Environment values are snapshotted on first use for the duration of
    // this compiler invocation; source import resolution never runs a shell.
    std::unordered_map<std::string, std::optional<std::string>> import_environment;
    bool owned, scalar_loop, host;
    std::uint32_t root_provider_bits = 0;
public:
    Loader(bool owned, bool scalar_loop, bool host,
           std::uint32_t admitted_provider_bits)
        : owned(owned), scalar_loop(scalar_loop), host(host),
          root_provider_bits(admitted_provider_bits) {}
    Program run(const std::string& path, std::string text, SourceIdentity identity) {
        read_bytes = text.size();
        visit(fs::canonical(path), std::move(text), identity, 0, false,
              frontend::CompilerLibraryTier::Core, root_provider_bits);
        // Defense in depth: the old module linker must never silently ignore
        // namespace AST records if a future parser starts admitting them.
        // Authenticated package-source ownership and typed namespace call
        // lowering have not been connected to this executable path.
        for (const auto& unit : units) {
            if (!unit->program.namespace_imports.empty())
                fail(*unit, "NAMESPACE_RUNTIME_BACKEND_UNSUPPORTED: native loader requires verified v2 namespace package linking");
        }
        if (units.front()->program.imports.empty()) {
            auto root = std::move(units.front()->program);
            resolve_and_validate_program(root);
            return root;
        }
        Program linked;
        linked.module = units.front()->program.module;
        linked.namespace_name = units.front()->program.namespace_name;
        for (std::size_t index = 0; index < units.size(); ++index) {
            auto& unit = *units[index];
            // The current linker transports ordinary scalar/class/actor
            // declarations only. An inverted owner must never disappear
            // while imported source units are combined.
            if (!unit.program.inverted_structs.empty() || unit.program.inverted_main)
                fail(unit, "DOP_INVERTED_CROSS_UNIT_UNSUPPORTED: linked inverted owner needs checked schema/owner identity");
            if (!unit.program.type_only_declarations.empty() ||
                    !unit.program.type_constraint_descriptors.empty() ||
                    !unit.program.namespace_name.empty()) {
                fail(unit,
                    "native linked type-only declarations/namespaces require checked cross-unit type identity lowering");
            }
            if (!unit.program.contracts.empty() || !unit.program.conforms.empty())
                fail(unit, "native linked contract conformance requires checked cross-unit contract identity lowering");
            // An init hook has observable ordering/once-only behavior in Java.
            // Never silently treat it as an ordinary never-called function.
            if (unit.functions.count("init")) fail(unit, "native module init hook requires initialization lowering");
            if (!unit.program.cooperative_loops.empty())
                fail(unit, "native linked suspend functions require module continuation lowering");
            if (index && !unit.program.actors.empty())
                fail(unit, "native imported actor declarations require typed module actor placement lowering");
            for (const auto& type : unit.program.sum_types) {
                try { merge_native_sum_descriptor(linked.sum_types, type); }
                catch (const std::runtime_error& error) { fail(unit, error.what()); }
            }
            for (auto& fn : unit.program.functions) {
                qualify_function(unit, fn);
                fn.name = unit.prefix + fn.name;
                fn.native_internal = index && !fn.exported;
                linked.functions.push_back(std::move(fn));
            }
            for (auto& cls : unit.program.classes) {
                binding_name(unit, cls.name);
                for (const auto& name : cls.constructor_parameters) binding_name(unit, name);
                for (auto& field : cls.fields) {
                    if (!field.class_name.empty()) field.class_name = class_type(unit, field.class_name);
                    if (!field.array_record_class.empty()) field.array_record_class = class_type(unit, field.array_record_class);
                    expression(unit, field.initializer_expression.get());
                }
                cls.name = unit.prefix + cls.name;
                linked.classes.push_back(std::move(cls));
            }
            // Root actors may call statically imported scalar helpers. Their
            // state/capability identities remain in the ordinary actor ABI.
            for (auto& actor : unit.program.actors) {
                for (auto& field : actor.fields) {
                    binding_name(unit, field.name);
                    expression(unit, field.initializer.get());
                }
                qualify_function(unit, actor.on_message);
                linked.actors.push_back(std::move(actor));
            }
        }
        resolve_and_validate_program(linked);
        return linked;
    }
private:
    [[noreturn]] static void fail(const Unit& unit, const std::string& reason) {
        throw std::runtime_error(unit.path.string() + ": " + reason);
    }
    static bool same_file(SourceIdentity left, SourceIdentity right) {
        return left.valid && right.valid && left.device == right.device && left.inode == right.inode;
    }
    std::size_t visit(fs::path path, std::string text, SourceIdentity identity,
                      std::size_t depth, bool compiler_owned_library,
                      frontend::CompilerLibraryTier compiler_library_tier,
                      std::uint32_t admitted_core_provider_bits) {
        if (depth > max_depth) throw std::runtime_error("native import graph exceeds 32 edges of depth");
        if (units.size() >= max_units) throw std::runtime_error("native import graph exceeds 128 source files");
        if (text.size() > max_bytes - bytes) throw std::runtime_error("native import graph exceeds 8 MiB aggregate source limit");
        bytes += text.size();
        require_valid_source_utf8(text);
        if (text.find("// ores:") != std::string::npos)
            throw std::runtime_error(path.string() + ": native target admission unsupported: ores environment headers require compiler verification");
        const auto index = units.size();
        auto unit = std::make_unique<Unit>();
        unit->path = std::move(path); unit->identity = identity;
        unit->prefix = index ? "__ores_module_" + std::to_string(index) + "." : "";
        unit->compiler_owned_library = compiler_owned_library;
        unit->compiler_library_tier = compiler_library_tier;
        unit->admitted_core_provider_bits = admitted_core_provider_bits;
        try { unit->program = parse_unresolved_program(text, owned, scalar_loop); }
        catch (const std::exception& error) {
            // Preserve historical diagnostics for a standalone entry unit.
            if (!index) throw;
            fail(*unit, error.what());
        }
        if (unit->compiler_owned_library) {
            unit->core_closure =
                frontend::infer_core_closure_requirements(unit->program);
            if (!frontend::core_closure_admitted(
                    unit->core_closure, unit->admitted_core_provider_bits))
                fail(*unit,
                    "compiler-owned library inferred undeclared provider or hosted effect");
        }
        for (const auto& fn : unit->program.functions) {
            auto& arities = unit->functions[fn.name];
            arities.emplace(fn.parameters.size(), fn.exported);
        }
        // The native compiler has no once-only startup barrier. Reject the
        // reserved lifecycle hook even for a standalone source file, before
        // the no-import fast path can compile it as an ordinary callable.
        if (unit->functions.count("init"))
            fail(*unit, "native module init hook requires once-only reachable initialization lowering");
        for (const auto& cls : unit->program.classes) {
            unit->classes.insert(cls.name); unit->class_exports.emplace(cls.name, cls.exported);
            unit->constructor_exports.emplace(cls.name, !cls.explicit_constructor || cls.constructor_exported);
        }
        paths.emplace(unit->path.string(), index);
        units.push_back(std::move(unit));
        auto& current = *units[index];
        if (!current.program.imports.empty() && scalar_loop)
            fail(current, "experimental scalar-loop compilation does not admit module imports");
        for (const auto& imported : current.program.imports) {
            if (++edges > 4096) fail(current, "native import graph exceeds 4096 edges");
            const auto& raw = imported.path;
            std::size_t selected = units.size();

            if (frontend::is_reserved_compiler_library_import(raw)) {
                const auto* library =
                    frontend::find_compiler_library_source(raw);
                if (!library) {
                    if (frontend::is_reserved_core_import(raw))
                        fail(current,
                            (raw.rfind("atto.core", 0) == 0
                                ? "unknown compiler-owned atto.core module '" :
                                  "unknown compiler-owned ores.core module '") + raw + "'");
                    fail(current,
                        (raw.rfind("atto.alloc", 0) == 0
                            ? "unknown compiler-owned atto.alloc module '" :
                              "unknown compiler-owned alloc module '") + raw + "'");
                }
                if (current.compiler_owned_library &&
                        !frontend::compiler_library_dependency_allowed(
                            current.compiler_library_tier, library->tier))
                    fail(current,
                        "compiler-owned ores.core modules may not depend on alloc/*");
                const std::string unit_id(library->unit_id);
                const auto known = paths.find(unit_id);
                selected = known == paths.end() ? units.size() : known->second;
                if (selected == index)
                    fail(current, "compiler-owned library module cannot import itself");
                if (selected == units.size()) {
                    if (library->source.size() > max_bytes - read_bytes)
                        fail(current,
                            "native import graph exceeds 8 MiB source read budget");
                    read_bytes += library->source.size();
                    selected = visit(
                        fs::path(unit_id), std::string(library->source),
                        SourceIdentity{}, depth + 1, true, library->tier,
                        library->admitted_provider_bits);
                }
                auto& target = *units[selected];
                if (target.program.module != library->module_name)
                    fail(current,
                        "compiler-owned library registry/module identity mismatch");
                if (!current.compiler_owned_library && !host &&
                        !frontend::core_closure_admitted(
                            target.core_closure, root_provider_bits)) {
                    const auto allocator =
                        frontend::core_provider_bit(
                            frontend::CoreProviderRequirement::Allocator);
                    if ((target.core_closure.provider_bits & allocator) != 0u &&
                            (root_provider_bits & allocator) == 0u)
                        fail(current,
                            "freestanding alloc/* requires an explicit allocator provider");
                    fail(current,
                        "freestanding compiler-owned library closure requires unavailable provider");
                }
                if (current.compiler_owned_library) {
                    if (!target.compiler_owned_library ||
                            !target.core_closure_complete)
                        fail(current,
                            "compiler-owned library import cycle or incomplete closure");
                    current.core_closure =
                        frontend::merge_core_closure_requirements(
                            current.core_closure, target.core_closure);
                    if (!frontend::core_closure_admitted(
                            current.core_closure,
                            current.admitted_core_provider_bits))
                        fail(current,
                            "compiler-owned library transitive provider requirement exceeds admitted providers");
                }
            } else {
                if (current.compiler_owned_library)
                    fail(current,
                        "compiler-owned library modules may import only ores.core/* or alloc/*");
                if (frontend::is_reserved_std_import(raw))
                    fail(current, raw.rfind("atto.std", 0) == 0
                        ? "native atto.std package resolution remains unsupported"
                        : "native ores.std package resolution remains unsupported");
                if (!host)
                    fail(current,
                        "freestanding native imports require compiler-owned ores.core/* modules");

                std::string resolved_raw;
                try {
                    resolved_raw = frontend::expand_static_import_path(
                        raw, [&](std::string_view name) -> std::optional<std::string> {
                            const std::string key(name);
                            if (const auto found = import_environment.find(key);
                                    found != import_environment.end())
                                return found->second;
                            std::optional<std::string> value;
                            if (const char* current = std::getenv(key.c_str()))
                                value = std::string(current);
                            return import_environment.emplace(key, std::move(value)).first->second;
                        });
                } catch (const std::exception& error) {
                    fail(current, error.what());
                }
                if (resolved_raw.rfind("https://", 0) == 0 ||
                        resolved_raw.rfind("http://", 0) == 0)
                    fail(current, "remote imports require locked Zed URL admission (not implemented)");
                // A compiler env variable cannot silently smuggle file://,
                // java:, ftp:, or another URI scheme into a local import.
                const auto colon = resolved_raw.find(':');
                if (colon != std::string::npos && colon > 0) {
                    const auto first = static_cast<unsigned char>(resolved_raw[0]);
                    bool scheme = (first >= 'a' && first <= 'z') ||
                                  (first >= 'A' && first <= 'Z');
                    for (std::size_t i = 1; scheme && i < colon; ++i) {
                        const auto ch = static_cast<unsigned char>(resolved_raw[i]);
                        scheme = (ch >= 'a' && ch <= 'z') ||
                                 (ch >= 'A' && ch <= 'Z') ||
                                 (ch >= '0' && ch <= '9') ||
                                 ch == '+' || ch == '.' || ch == '-';
                    }
                    if (scheme)
                        fail(current, "unsupported import URI scheme");
                }
                fs::path dependency;
                if (resolved_raw.rfind("+/", 0) == 0) {
                    try {
                        dependency = resolve_project_root_import(
                            current.path, resolved_raw,
                            [](const fs::path& manifest) {
                                const auto manifest_path = manifest.string();
                                const auto expected_manifest = capture_source_identity(
                                    manifest_path.c_str(), 64u * 1024u,
                                    "native package manifest exceeds 64 KiB");
                                auto bytes = read_regular_source(
                                    manifest_path.c_str(), 64u * 1024u,
                                    "native package manifest exceeds 64 KiB",
                                    nullptr, &expected_manifest);
                                require_valid_source_utf8(bytes);
                                return bytes;
                            });
                    } catch (const std::exception& error) {
                        fail(current, error.what());
                    }
                } else {
                    const fs::path source_path(resolved_raw);
                    const bool absolute_from_env = source_path.is_absolute();
                    if (!absolute_from_env &&
                            resolved_raw.rfind("./", 0) != 0 &&
                            resolved_raw.rfind("../", 0) != 0)
                        fail(current, "native imports require relative ./ or ../, project-root +/, or an expanded absolute ${NAME} path");
                    if (absolute_from_env && raw.find("${") == std::string::npos)
                        fail(current, "native absolute imports require explicit ${NAME} interpolation");
                    dependency = project_root_import_detail::resolve_native_source_suffix(
                        absolute_from_env ? source_path : current.path.parent_path() / source_path);
                    std::error_code error;
                    dependency = fs::canonical(dependency, error);
                    if (error) fail(current, "cannot resolve native import '" + raw + "'");
                    try {
                        project_root_import_detail::enforce_relative_import_package_boundary(
                            current.path, dependency, absolute_from_env);
                    } catch (const std::exception& boundary_error) {
                        fail(current, boundary_error.what());
                    }
                }

                const auto known = paths.find(dependency.string());
                selected = known == paths.end() ? units.size() : known->second;
                if (selected == index) fail(current, "source file cannot import itself");
                if (selected == units.size()) {
                    const auto dependency_path = dependency.string();
                    const auto expected_child_identity = capture_source_identity(
                        dependency_path.c_str(), max_bytes - read_bytes,
                        "native import graph exceeds 8 MiB source read budget");
                    SourceIdentity child_identity;
                    auto child_text = read_regular_source(
                        dependency_path.c_str(), max_bytes - read_bytes,
                        "native import graph exceeds 8 MiB source read budget",
                        &child_identity, &expected_child_identity);
                    read_bytes += child_text.size();
                    // Compare descriptor identities too: hard-link aliases must
                    // not bypass self-edge rejection or duplicate unit identity.
                    for (std::size_t i = 0; i < units.size(); ++i) {
                        if (same_file(units[i]->identity, child_identity)) {
                            selected = i;
                            break;
                        }
                    }
                    if (selected == index)
                        fail(current, "source file cannot import itself through a filesystem alias");
                    if (selected == units.size())
                        selected = visit(
                            dependency, std::move(child_text), child_identity,
                            depth + 1, false,
                            frontend::CompilerLibraryTier::Core, 0u);
                    else
                        paths.emplace(dependency.string(), selected);
                }
            }
            if (imported.local_name == current.program.module)
                fail(current, "native imported module binding conflicts with containing module");
            const auto& target = *units[selected];
            if (imported.selected_function) {
                const auto found = target.functions.find(imported.selected_module);
                bool exported = false;
                if (found != target.functions.end())
                    for (const auto& arity : found->second) exported = exported || arity.second;
                if (!exported)
                    fail(current, "unknown or private imported function '" + imported.selected_module + "'");
                if (current.functions.count(imported.local_name))
                    fail(current, "imported function conflicts with local function '" + imported.local_name + "'");
                current.function_imports.emplace(imported.local_name, std::make_pair(selected, imported.selected_module));
                continue;
            }
            if (imported.selected_class) {
                const auto cls = target.class_exports.find(imported.selected_module);
                if (cls == target.class_exports.end() || !cls->second)
                    fail(current, "unknown or private imported class '" + imported.selected_module + "'");
                current.class_imports.emplace(imported.local_name, std::make_pair(selected, imported.selected_module));
            } else if (target.program.module != imported.selected_module)
                fail(current, "imported file does not declare selected module '" + imported.selected_module + "'");
            if (!imported.selected_class)
                current.dependencies.emplace(imported.local_name, selected);
        }
        if (current.compiler_owned_library) {
            if (!frontend::core_closure_admitted(
                    current.core_closure,
                    current.admitted_core_provider_bits))
                fail(current,
                    "compiler-owned library closure exceeds admitted providers");
            current.core_closure_complete = true;
        }
        return index;
    }
    static void binding_name(const Unit& unit, const std::string& name) {
        if (unit.dependencies.count(name) || unit.class_imports.count(name) || unit.function_imports.count(name))
            fail(unit, "native module import binding cannot be shadowed: '" + name + "'");
    }
    void expression(const Unit& unit, Expression* expr) {
        if (!expr) return;
        expression(unit, expr->left.get()); expression(unit, expr->right.get());
        expression(unit, expr->condition.get()); expression(unit, expr->condition_lhs.get()); expression(unit, expr->condition_rhs.get());
        expression(unit, expr->when_true.get()); expression(unit, expr->when_false.get());
        for (auto& arg : expr->arguments) expression(unit, arg.get());
        if (expr->kind == Expression::Kind::RecordConstruct) {
            expr->name = class_type(unit, expr->name, true);
            return;
        }
        if (expr->kind != Expression::Kind::Call) return;
        const auto dot = expr->name.find('.');
        const Unit* target = &unit;
        std::string member = expr->name;
        bool external = false;
        if (dot != std::string::npos) {
            const auto binding = unit.dependencies.find(expr->name.substr(0, dot));
            if (binding == unit.dependencies.end()) fail(unit, "unknown native module binding");
            target = units[binding->second].get(); member = expr->name.substr(dot + 1); external = true;
        } else if (!unit.functions.count(member)) {
            const auto imported = unit.function_imports.find(member);
            if (imported != unit.function_imports.end()) {
                target = units[imported->second.first].get(); member = imported->second.second; external = true;
            }
        }
        const auto function = target->functions.find(member);
        if (function == target->functions.end() || !function->second.count(expr->arguments.size()))
            fail(unit, "unknown function or wrong call arity '" + expr->name +
                (function == target->functions.end() ? "': unknown function" : "': call arity mismatch"));
        if (external && !function->second.at(expr->arguments.size()))
            fail(unit, "imported function is private: '" + expr->name + "'");
        expr->name = target->prefix + member;
    }
    std::string class_type(const Unit& unit, const std::string& name, bool constructing = false) {
        const auto imported = unit.class_imports.find(name);
        if (imported != unit.class_imports.end()) {
            const auto& target = *units[imported->second.first];
            if (constructing && !target.constructor_exports.at(imported->second.second))
                fail(unit, "private imported constructor is not accessible");
            return target.prefix + imported->second.second;
        }
        if (unit.classes.count(name)) return unit.prefix + name;
        const auto dot = name.find('.');
        if (dot == std::string::npos) return name;
        const auto dependency = unit.dependencies.find(name.substr(0, dot));
        if (dependency == unit.dependencies.end()) fail(unit, "unknown module in class type");
        const auto& target = *units[dependency->second];
        auto member = target.class_exports.find(name.substr(dot + 1));
        if (member == target.class_exports.end() || !member->second)
            fail(unit, "unknown or private imported class type '" + name + "'");
        if (constructing && !target.constructor_exports.at(member->first))
            fail(unit, "private imported constructor is not accessible");
        return target.prefix + member->first;
    }
    void local(const Unit& unit, LocalBinding& binding) {
        binding_name(unit, binding.name);
        if (!binding.class_name.empty()) binding.class_name = class_type(unit, binding.class_name, binding.new_class);
        if (!binding.array_record_class.empty()) binding.array_record_class = class_type(unit, binding.array_record_class);
        expression(unit, binding.initializer.get());
        for (auto& arg : binding.constructor_arguments) expression(unit, arg.get());
    }
    void statement(const Unit& unit, Statement& stmt) {
        if (stmt.branch_local) local(unit, *stmt.branch_local);
        expression(unit, stmt.expression.get());
        expression(unit, stmt.condition.get()); expression(unit, stmt.condition_lhs.get()); expression(unit, stmt.condition_rhs.get());
        expression(unit, stmt.loop_step.get());
        expression(unit, stmt.array_commit_length.get());
        for (auto& value : stmt.array_commit_values) expression(unit, value.get());
        for (auto& arm : stmt.then_statements) statement(unit, arm);
        for (auto& arm : stmt.else_statements) statement(unit, arm);
    }
    void qualify_function(const Unit& unit, Function& fn) {
        binding_name(unit, fn.name);
        if (!fn.return_class.empty()) fn.return_class = class_type(unit, fn.return_class);
        if (!fn.method_owner.empty()) fn.method_owner = class_type(unit, fn.method_owner);
        for (auto& parameter : fn.parameters) {
            binding_name(unit, parameter.name);
            if (!parameter.class_name.empty()) parameter.class_name = class_type(unit, parameter.class_name);
        }
        for (auto& binding : fn.locals) local(unit, binding);
        for (auto& stmt : fn.statements) statement(unit, stmt);
        for (auto& stmt : fn.caught_throw_defers) statement(unit, stmt);
        for (auto& guard : fn.early_returns) {
            expression(unit, guard.condition.get()); expression(unit, guard.lhs.get()); expression(unit, guard.rhs.get()); expression(unit, guard.result.get());
        }
        for (auto& send : fn.actor_sends) expression(unit, send.payload.get());
        for (auto& write : fn.actor_field_writes) expression(unit, write.expression.get());
        for (const auto& spawn : fn.actor_spawns) binding_name(unit, spawn.name);
        if (!fn.catch_binding.empty()) binding_name(unit, fn.catch_binding);
        expression(unit, fn.caught_throw_value.get()); expression(unit, fn.result.get());
    }
};
} // namespace
Program load_native_program(const std::string& path, std::string source, SourceIdentity identity,
                            bool owned, bool scalar_loop, bool host,
                            std::uint32_t admitted_provider_bits) {
    return Loader(owned, scalar_loop, host, admitted_provider_bits)
        .run(path, std::move(source), identity);
}
frontend::VerifiedNamespaceSourceSnapshotV2 admit_native_namespace_sources_v2(
    const std::filesystem::path& trusted_physical_root,
    const std::vector<std::string>& approved_members,
    std::string_view approved_manifest_sha256,
    std::string_view approved_sources_sha256,
    const frontend::NamespaceSha256& trusted_sha256) {
    // Only the future authenticated package/lock host may provide these
    // arguments. This host preparatory API does not bypass the executable
    // native module loader's fail-closed namespace import gate.
    return frontend::load_verified_namespace_source_snapshot_v2(
        trusted_physical_root, approved_members, approved_manifest_sha256,
        approved_sources_sha256, trusted_sha256);
}
} // namespace oreslang
