#include "oreslang/frontend/ModuleLoader.hpp"
#include "oreslang/frontend/CoreLibrarySources.hpp"
#include "oreslang/frontend/NativeLogicalSourceAdmission.hpp"
#include "oreslang/frontend/ProjectRootImport.hpp"
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
    std::string logical_source_id;
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
    // Only namespace-star imports may provide compiler-time macro authority.
    std::unordered_set<std::string> macro_namespace_bindings;
    std::unordered_map<std::string, std::pair<std::size_t, std::string>> class_imports;
    // Declaration identities stay immutable while ASTs are moved/qualified.
    std::unordered_map<std::string, std::unordered_map<std::size_t, bool>> functions;
    std::unordered_set<std::string> classes;
    std::unordered_map<std::string, bool> class_exports;
    std::unordered_map<std::string, bool> constructor_exports;
};
class Loader {
    std::vector<std::unique_ptr<Unit>> units;
    std::size_t bytes = 0, read_bytes = 0, edges = 0;
    // The 128-expansion ceiling is per admitted consumer source unit,
    // matching the per-unit Java macro expansion pass. A graph-global scalar
    // would reject two independent units each using 128 permitted macros.
    std::unordered_map<std::string, std::size_t> imported_macro_expansions_by_unit;
    std::unordered_map<std::string, std::size_t> paths;
    std::unique_ptr<frontend::NativeLogicalSourceAdmission> source_admission;
    bool owned, scalar_loop, host;
    std::uint32_t root_provider_bits = 0;
public:
    Loader(bool owned, bool scalar_loop, bool host,
           std::uint32_t admitted_provider_bits)
        : owned(owned), scalar_loop(scalar_loop), host(host),
          root_provider_bits(admitted_provider_bits) {}
    Program run(const std::string& path, std::string text, SourceIdentity identity) {
        // Validate the caller-spelled entry before canonical() erases aliases.
        source_admission = std::make_unique<frontend::NativeLogicalSourceAdmission>(path);
        source_admission->admit(fs::path(path), identity);
        read_bytes = text.size();
        visit(fs::canonical(path), std::move(text), identity, 0, false,
              frontend::CompilerLibraryTier::Core, root_provider_bits);
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
                bool found = false; for (const auto& existing : linked.sum_types) if (existing.name == type.name) found = true;
                if (!found) linked.sum_types.push_back(type);
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
        if (!compiler_owned_library) {
            unit->logical_source_id = source_admission->admit(unit->path, identity);
            // The descriptor snapshot supplied by the bounded reader must
            // still identify the same object when compiler parsing begins.
            const auto physical_name = unit->path.string();
            const auto current_identity = capture_source_identity(
                physical_name.c_str(), max_bytes,
                "native source exceeds 8 MiB identity check budget");
            if (!ores_source_identity_equal(identity, current_identity))
                throw std::runtime_error("native source identity changed after bounded read");
        }
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
        for (const auto& cls : unit->program.classes) {
            unit->classes.insert(cls.name); unit->class_exports.emplace(cls.name, cls.exported);
            unit->constructor_exports.emplace(cls.name, !cls.explicit_constructor || cls.constructor_exported);
        }
        // Imported source modules are indexed by canonical logical unit ID;
        // only compiler-owned virtual core libraries retain registry keys.
        paths.emplace(unit->compiler_owned_library
            ? unit->path.string() : unit->logical_source_id, index);
        units.push_back(std::move(unit));
        auto& current = *units[index];
        if (!current.program.imports.empty() && scalar_loop)
            fail(current, "experimental scalar-loop compilation does not admit module imports");
        for (const auto& imported : current.program.imports) {
            if (++edges > 4096) fail(current, "native import graph exceeds 4096 edges");
            // Match Java's admission ordering: depth denial precedes import
            // path resolution and any host filesystem identity I/O.
            if (depth >= max_depth)
                fail(current, "native import graph exceeds 32 edges of depth");
            const auto& raw = imported.path;
            std::size_t selected = units.size();

            if (frontend::is_reserved_compiler_library_import(raw)) {
                const auto* library =
                    frontend::find_compiler_library_source(raw);
                if (!library) {
                    if (frontend::is_reserved_core_import(raw))
                        fail(current,
                            "unknown compiler-owned ores.core module '" + raw + "'");
                    fail(current,
                        "unknown compiler-owned alloc module '" + raw + "'");
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
                    fail(current, "native ores.std package resolution remains unsupported");
                if (!host)
                    fail(current,
                        "freestanding native imports require compiler-owned ores.core/* modules");

                fs::path dependency;
                if (raw.rfind("+/", 0) == 0) {
                    try {
                        dependency = resolve_project_root_import(
                            current.path, raw,
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
                    if (raw.rfind("./", 0) != 0 && raw.rfind("../", 0) != 0)
                        fail(current, "native imports require relative ./ or ../, project-root +/, or compiler-owned ores.core/* source paths");
                    // Check root ownership *before* extensionless suffix
                    // discovery (which stats .atto/.ores candidates). Outside
                    // paths must be rejected regardless of whether they exist.
                    const auto unresolved = current.path.parent_path() / fs::path(raw);
                    // Reject symlinked ancestor directories too, before
                    // resolve_native_source_suffix probes either file name.
                    // The lexical gate alone cannot stop an in-root alias
                    // directory redirecting exists_marker() outside the root.
                    frontend::logical_source_detail::no_symlink_components(
                        unresolved, source_admission->root());
                    dependency = project_root_import_detail::resolve_native_source_suffix(
                        unresolved, project_root_import_detail::is_atto_source(current.path));
                    // Recheck the actual suffixed path and reject symlinks
                    // before canonicalization discards the caller spelling.
                    frontend::logical_source_detail::no_symlink_components(
                        dependency, source_admission->root());
                    std::error_code error;
                    dependency = fs::canonical(dependency, error);
                    if (error) fail(current, "cannot resolve native import '" + raw + "'");
                    try {
                        project_root_import_detail::enforce_relative_import_package_boundary(
                            current.path, dependency);
                    } catch (const std::exception& boundary_error) {
                        fail(current, boundary_error.what());
                    }
                }

                const auto dependency_path = dependency.string();
                // Already-admitted cycle/backedges do not consume a fresh
                // byte quota. Enforce the remaining limit only on a new read.
                const auto expected_child_identity = capture_source_identity(
                    dependency_path.c_str(), max_bytes,
                    "native import graph exceeds 8 MiB source read budget");
                const auto logical_dependency =
                    source_admission->admit(dependency, expected_child_identity);
                const auto known = paths.find(logical_dependency);
                // The real compiler's graph is keyed by the same root-relative
                // identity that its parser/source-unit registry admitted.
                selected = known == paths.end() ? units.size() : known->second;
                if (selected == index) fail(current, "source file cannot import itself");
                if (selected == units.size()) {
                    if (expected_child_identity.size > max_bytes - read_bytes)
                        fail(current, "native import graph exceeds 8 MiB source read budget");
                    SourceIdentity child_identity;
                    auto child_text = read_regular_source(
                        dependency_path.c_str(), max_bytes - read_bytes,
                        "native import graph exceeds 8 MiB source read budget",
                        &child_identity, &expected_child_identity);
                    read_bytes += child_text.size();
                    // A second filename for the same descriptor is not a new
                    // source owner. Never silently collapse hardlink aliases.
                    for (std::size_t i = 0; i < units.size(); ++i) {
                        if (same_file(units[i]->identity, child_identity)) {
                            if (units[i]->path != dependency)
                                fail(current, "native source file alias has multiple logical IDs");
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
                        // Aliases never acquire a second logical graph key.
                        paths.emplace(logical_dependency, selected);
                }
            }
            if (imported.local_name == current.program.module)
                fail(current, "native imported module binding conflicts with containing module");
            const auto& target = *units[selected];
            if (imported.macro_namespace) {
                // v1 import * is NOT a runtime wildcard import. It only
                // admits public, statically checked macro declarations from
                // a single project-owned .atto producer with no runtime ABI.
                if (target.compiler_owned_library || target.logical_source_id.empty() ||
                    target.path.extension() != ".atto" ||
                    target.program.native_macros.empty() ||
                    !target.program.imports.empty() ||
                    !target.program.functions.empty() || !target.program.classes.empty() ||
                    !target.program.actors.empty() || !target.program.generators.empty() ||
                    !target.program.type_only_declarations.empty() ||
                    !target.program.module.empty())
                    fail(current, "native macro namespace requires a standalone macro-only .atto source");
                current.macro_namespace_bindings.insert(imported.local_name);
            } else if (imported.selected_class) {
                const auto cls = target.class_exports.find(imported.selected_module);
                if (cls == target.class_exports.end() || !cls->second)
                    fail(current, "unknown or private imported class '" + imported.selected_module + "'");
                current.class_imports.emplace(imported.local_name, std::make_pair(selected, imported.selected_module));
            } else if (!imported.macro_namespace &&
                       target.program.module != imported.selected_module)
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
        if (unit.dependencies.count(name) || unit.class_imports.count(name))
            fail(unit, "native module import binding cannot be shadowed: '" + name + "'");
    }
    static std::int64_t imported_macro_const(const Unit& unit, const Expression& node,
                                             std::size_t depth, std::size_t& visited) {
        if (++visited > 512 || depth > 64)
            fail(unit, "NATIVE_MACRO_BUDGET: imported const AST exceeds 512 nodes or 64 depth");
        using K = Expression::Kind;
        if (node.kind == K::Integer) return node.integer;
        std::int64_t a = 0, b = 0, out = 0;
        if (node.kind == K::Negate && node.left) {
            a = imported_macro_const(unit, *node.left, depth + 1, visited);
            if (__builtin_sub_overflow(std::int64_t{0}, a, &out))
                fail(unit, "NATIVE_MACRO_CONST_OVERFLOW: imported macro arithmetic overflow");
            return out;
        }
        if ((node.kind == K::Add || node.kind == K::Subtract ||
             node.kind == K::Multiply) && node.left && node.right) {
            a = imported_macro_const(unit, *node.left, depth + 1, visited);
            b = imported_macro_const(unit, *node.right, depth + 1, visited);
            const bool overflow = node.kind == K::Add
                ? __builtin_add_overflow(a, b, &out)
                : node.kind == K::Subtract
                ? __builtin_sub_overflow(a, b, &out)
                : __builtin_mul_overflow(a, b, &out);
            if (overflow)
                fail(unit, "NATIVE_MACRO_CONST_OVERFLOW: imported macro arithmetic overflow");
            return out;
        }
        fail(unit, "NATIVE_MACRO_CONST_UNCLOSED: imported Const<int> must be closed typed integer AST");
    }

    void expand_imported_macro(const Unit& unit, Expression& expr) {
        const auto dot = expr.name.find('.');
        if (dot == std::string::npos || expr.name.find('.', dot + 1) != std::string::npos)
            fail(unit, "NATIVE_MACRO_EXPANDER_REQUIRED: imported macro needs exact namespace.member");
        const auto alias = expr.name.substr(0, dot);
        if (!unit.macro_namespace_bindings.count(alias))
            fail(unit, "NATIVE_MACRO_PROVENANCE: source is not an admitted macro namespace import");
        const auto binding = unit.dependencies.find(alias);
        if (binding == unit.dependencies.end())
            fail(unit, "NATIVE_MACRO_PROVENANCE: imported macro has no bound source owner");
        const auto& producer = *units[binding->second];
        if (producer.compiler_owned_library || producer.logical_source_id.empty() ||
            !producer.identity.valid || !unit.identity.valid)
            fail(unit, "NATIVE_MACRO_PROVENANCE: imported macro lacks verified source owner");
        const auto name = expr.name.substr(dot + 1);
        const NativeMacroDecl* exported = nullptr;
        for (const auto& declaration : producer.program.native_macros) {
            if (declaration.name == name) { exported = &declaration; break; }
        }
        if (!exported || !exported->exported)
            fail(unit, "NATIVE_MACRO_PRIVATE: unknown or private imported macro");
        const auto kind = exported->kind;
        const bool const_quote = kind == NativeMacroDecl::Kind::ConstIntQuote;
        const bool syntax_identity = kind == NativeMacroDecl::Kind::SyntaxExprIdentity;
        const bool syntax_twice = kind == NativeMacroDecl::Kind::SyntaxExprLiteralTwice;
        if ((!const_quote && !syntax_identity && !syntax_twice) ||
            expr.imported_macro_const_argument != const_quote ||
            expr.arguments.size() != 1 || !expr.arguments[0])
            fail(unit, "NATIVE_MACRO_EXPANDER_REQUIRED: imported macro signature/argument unsupported");
        if (unit.logical_source_id.empty())
            fail(unit, "NATIVE_MACRO_PROVENANCE: consumer source has no admitted logical identity");
        // Initialize each admitted source unit from the parser's local
        // expansion count. Imported and local macros share ONE portable
        // 128-expansion ceiling; neither path gets an additional quota.
        auto& unit_expansions = imported_macro_expansions_by_unit.try_emplace(
            unit.logical_source_id,
            unit.program.native_macro_expansions).first->second;
        if (++unit_expansions > 128)
            fail(unit, "NATIVE_MACRO_BUDGET: imported expansion count exceeds 128 per source unit");
        if (syntax_identity) {
            // Move the original caller AST exactly once: no duplicated
            // effects, mutable loans, captures, or additional evaluation.
            // Subexpressions were already qualified before this call.
            auto forwarded = std::move(expr.arguments.front());
            expr = std::move(*forwarded);
            return;
        }
        if (syntax_twice) {
            // Conservative typed-template parity with the local expander:
            // the duplicated slot must be a single closed i64 literal. A
            // variable, call, borrow, or effectful expression must never be
            // evaluated twice by source expansion.
            if (expr.arguments.front()->kind != Expression::Kind::Integer)
                fail(unit, "NATIVE_MACRO_EXPANDER_REQUIRED: repeated unquote requires a closed integer literal");
            const auto value = expr.arguments.front()->integer;
            std::int64_t checked_sum = 0;
            if (__builtin_add_overflow(value, value, &checked_sum))
                fail(unit, "NATIVE_MACRO_CONST_OVERFLOW: repeated imported literal exceeds signed i64");
            const auto location = expr.arguments.front()->offset;
            auto first = std::make_unique<Expression>();
            first->kind = Expression::Kind::Integer;
            first->integer = value;
            first->offset = location;
            auto second = std::make_unique<Expression>();
            second->kind = Expression::Kind::Integer;
            second->integer = value;
            second->offset = location;
            expr.kind = Expression::Kind::Add;
            expr.left = std::move(first);
            expr.right = std::move(second);
        } else {
            std::size_t visited = 0;
            const auto constant = imported_macro_const(unit, *expr.arguments[0], 0, visited);
            // The single typed literal is a fresh AST result and retains no
            // generated identifier, host path or runtime macro invocation.
            expr.kind = Expression::Kind::Integer;
            expr.integer = constant;
        }
        expr.name.clear();
        expr.arguments.clear();
        expr.argument_names.clear();
        expr.imported_macro_const_argument = false;
        expr.imported_macro_pending = false;
    }

    void expression(const Unit& unit, Expression* expr) {
        if (!expr) return;
        expression(unit, expr->left.get()); expression(unit, expr->right.get());
        expression(unit, expr->condition.get()); expression(unit, expr->condition_lhs.get()); expression(unit, expr->condition_rhs.get());
        expression(unit, expr->when_true.get()); expression(unit, expr->when_false.get());
        for (auto& arg : expr->arguments) expression(unit, arg.get());
        if (expr->imported_macro_pending) {
            expand_imported_macro(unit, *expr);
            return;
        }
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
            if (unit.macro_namespace_bindings.count(expr->name.substr(0, dot)))
                fail(unit, "NATIVE_MACRO_PROVENANCE: macro namespace cannot expose runtime functions");
            const auto binding = unit.dependencies.find(expr->name.substr(0, dot));
            if (binding == unit.dependencies.end()) fail(unit, "unknown native module binding");
            target = units[binding->second].get(); member = expr->name.substr(dot + 1); external = true;
        }
        const auto function = target->functions.find(member);
        if (function == target->functions.end() || !function->second.count(expr->arguments.size()))
            fail(unit, "unknown function or wrong call arity '" + expr->name + "'");
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
} // namespace oreslang
