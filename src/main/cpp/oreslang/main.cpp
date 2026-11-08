#include "oreslang/backend/LlvmCodegen.hpp"
#include "oreslang/frontend/Parser.hpp"
#include "oreslang/frontend/SourceInput.hpp"
#include "oreslang/frontend/CallableSelector.hpp"
#include "oreslang/frontend/ModuleLoader.hpp"
#include "oreslang/frontend/LibraryProfile.hpp"
#include "oreslang/frontend/CoreLibrarySources.hpp"
#include "oreslang/runtime/TargetAbi.hpp"
#include "oreslang/runtime/CoreLog.hpp"
#include "oreslang/types/RealtimeAnalysis.hpp"

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <new>
#include <fstream>
#include <filesystem>
#include <functional>
#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <iterator>
#include <stdexcept>
#include <string>
#include <array>
#include <string_view>


// Compiler failures can include guest-controlled tokens and filenames.
// Never emit terminal control bytes, unbounded exception text or the exact
// source filesystem path in a native compiler diagnostic.
static void print_safe_diagnostic(std::ostream& stream, std::string_view message,
                                  std::size_t limit) {
    const auto count = std::min(message.size(), limit);
    for (std::size_t i = 0; i < count; ++i) {
        const unsigned char byte = static_cast<unsigned char>(message[i]);
        stream.put(byte >= 32 && byte < 127 ? static_cast<char>(byte) : '?');
    }
    if (message.size() > limit) stream << "...[truncated]";
}


static bool paths_alias(const char* source_path, const std::string& report_path) {
    std::error_code source_error;
    std::error_code report_error;
    const auto source = std::filesystem::weakly_canonical(
        std::filesystem::path(source_path), source_error);
    const auto report = std::filesystem::weakly_canonical(
        std::filesystem::path(report_path), report_error);
    if (source_error || report_error)
        throw std::runtime_error("cannot safely resolve rt-report/source paths");
    if (source == report) return true;
    std::error_code identity_error;
    const bool same_file = std::filesystem::equivalent(source, report, identity_error);
    if (!identity_error) return same_file;
    // libc++ may report a generic identity error when the destination does
    // not exist yet. Check that case explicitly without swallowing failures.
    std::error_code exists_error;
    const bool report_exists = std::filesystem::exists(report, exists_error);
    if (!exists_error && !report_exists) return false;
    if (identity_error == std::errc::no_such_file_or_directory) return false;
    throw std::runtime_error("cannot safely compare rt-report/source identity");
}

static std::vector<std::string> tree_shake_function_calls(
        const oreslang::Function& function,
        std::size_t remaining_budget) {
    const auto facts =
            oreslang::realtime_detail::direct_facts(function, remaining_budget);
    if (facts.call_budget_exceeded)
        throw std::runtime_error(
            "tree shaking call graph exceeds compiler edge budget");
    return facts.calls;
}

static void require_function_only_tree_shake_surface(
        const oreslang::Program& program) {
    if (!program.actors.empty())
        throw std::runtime_error(
            "tree shaking currently rejects actor declarations until actor roots are modeled");
    if (!program.classes.empty())
        throw std::runtime_error(
            "tree shaking currently rejects class declarations until class initializer roots are modeled");
    if (!program.cooperative_loops.empty())
        throw std::runtime_error(
            "tree shaking currently rejects cooperative loop declarations until continuation ABI roots are modeled");
    if (std::any_of(
            program.functions.begin(),
            program.functions.end(),
            [](const oreslang::Function& fn) {
                return static_cast<bool>(fn.scalar_while);
            }))
        throw std::runtime_error(
            "tree shaking currently rejects scalar continuation functions until continuation ABI roots are modeled");
    if (std::any_of(
            program.functions.begin(),
            program.functions.end(),
            [](const oreslang::Function& fn) {
                return fn.async_source_i64 ||
                    !fn.source_tasks.empty() ||
                    !fn.future_pipelines.empty();
            }))
        throw std::runtime_error(
            "tree shaking currently rejects Task/Future pipeline metadata edges until they participate in the semantic reachability graph");
}

static void tree_shake_functions(oreslang::Program& program, const std::vector<std::string>& requested_roots) {
    std::unordered_map<std::string, std::size_t> definitions;
    for (std::size_t i = 0; i < program.functions.size(); ++i) {
        const auto& function = program.functions[i];
        const std::string key =
            oreslang::realtime_detail::function_key(
                function.name, function.parameters.size());
        if (!definitions.emplace(key, i).second)
            throw std::runtime_error(
                "tree shaking requires unique native function name/arity identities");
    }

    std::vector<std::string> pending;
    std::unordered_set<std::string> reachable;
    if (!requested_roots.empty()) {
        for (const auto& requested : requested_roots) {
            std::vector<std::string> matches;
            for (const auto& function : program.functions) {
                if (function.name == requested)
                    matches.push_back(oreslang::realtime_detail::function_key(
                        function.name, function.parameters.size()));
            }
            if (matches.empty())
                throw std::runtime_error("unknown tree-shake root: " + requested);
            if (matches.size() != 1)
                throw std::runtime_error("ambiguous tree-shake root: " + requested);
            if (reachable.insert(matches.front()).second) pending.push_back(matches.front());
        }
    } else {
        for (const auto& function : program.functions) {
            if (function.exported || function.name == "main") {
                const std::string key =
                    oreslang::realtime_detail::function_key(
                        function.name, function.parameters.size());
                if (reachable.insert(key).second) pending.push_back(key);
            }
            if (function.exported) {
                // LLVM names overloaded families according to family
                // cardinality. Preserve every sibling of a public overload in
                // conservative mode so removing a private sibling cannot
                // silently rename the surviving exported ABI symbol.
                for (const auto& sibling : program.functions) {
                    if (sibling.name != function.name) continue;
                    const std::string sibling_key =
                        oreslang::realtime_detail::function_key(
                            sibling.name, sibling.parameters.size());
                    if (reachable.insert(sibling_key).second)
                        pending.push_back(sibling_key);
                }
            }
        }
    }
    if (pending.empty())
        throw std::runtime_error(
            "tree shaking requires an explicit root, main, or at least one pub function");

    std::size_t scanned_edges = 0;
    for (std::size_t cursor = 0; cursor < pending.size(); ++cursor) {
        const auto definition = definitions.find(pending[cursor]);
        if (definition == definitions.end())
            throw std::runtime_error("tree shaking found an unresolved function root");
        if (scanned_edges >= oreslang::realtime_detail::kMaxRealtimeCallEdges)
            throw std::runtime_error(
                "tree shaking call graph exceeds compiler edge budget");
        const auto calls = tree_shake_function_calls(
                program.functions[definition->second],
                oreslang::realtime_detail::kMaxRealtimeCallEdges - scanned_edges);
        scanned_edges += calls.size();
        for (const auto& call : calls) {
            if (!definitions.count(call))
                throw std::runtime_error(
                    "tree shaking found an unresolved direct call: " + call);
            if (reachable.insert(call).second) pending.push_back(call);
        }
    }

    program.functions.erase(
        std::remove_if(program.functions.begin(), program.functions.end(),
            [&](const oreslang::Function& function) {
                return !reachable.count(oreslang::realtime_detail::function_key(
                    function.name, function.parameters.size()));
            }),
        program.functions.end());
}



// This deliberately narrow strict AOT admission path proves *static scalar
// call graph closure* before LLVM IR emission. It is not proof of actor heaps,
// complete guest AOT, whole-program no-GC, WCET, or linker/FFI reachability.
// Reuse the same compiler-maintained expression/statement scanner as the
// hard-RT analysis; never trust source-text keyword scans as a graph oracle.
static void require_sealed_scalar_aot_graph(const oreslang::Program& program,
                                             bool allow_class_fields = false) {
    using namespace oreslang;
    namespace detail = oreslang::realtime_detail;
    if (!program.actors.empty() ||
        (!allow_class_fields && !program.classes.empty()) ||
        !program.generators.empty() || !program.cooperative_loops.empty() ||
        !program.imports.empty() || !program.contracts.empty() ||
        !program.type_only_declarations.empty() || !program.sum_types.empty() ||
        !program.conforms.empty() || !program.type_constraint_descriptors.empty())
        throw std::runtime_error(
            "sealed scalar AOT rejects non-scalar declarations until their "
            "dispatch, initialization, continuation and drop roots are verified");

    // Only immutable literal scalar class fields are admitted in this
    // subprofile; no method dispatch, owned arrays, mutable state, nested
    // aggregates or unmodeled constructor side effects.
    std::unordered_set<std::string> admitted_classes;
    if (allow_class_fields) {
        if (program.classes.size() > 32)
            throw std::runtime_error("sealed class-field AOT class limit exceeded");
        for (const auto& cls : program.classes) {
            if (!admitted_classes.insert(cls.name).second)
                throw std::runtime_error("sealed class-field AOT duplicate nominal class");
            if (!cls.methods.empty() || cls.explicit_constructor ||
                !cls.constructor_parameters.empty() ||
                !cls.constructor_parameter_types.empty())
                throw std::runtime_error(
                    "sealed class-field AOT rejects methods or parameterized constructors");
            if (cls.fields.empty() || cls.fields.size() > 16)
                throw std::runtime_error("sealed class-field AOT requires 1-16 static fields");
            for (const auto& field : cls.fields) {
                const auto* initial = field.initializer_expression.get();
                if (!field.final_field || field.mutable_referent || field.int_array ||
                    !field.class_name.empty() || !field.sum_type.empty() ||
                    field.constructor_argument || field.scalar_type == ScalarType::String ||
                    !initial ||
                    (initial->kind != Expression::Kind::Integer &&
                     initial->kind != Expression::Kind::Boolean) ||
                    // A literal is a leaf in the checked AST. The kind alone
                    // is not an authorization to ignore future child edges.
                    initial->left || initial->right || initial->condition ||
                    initial->condition_lhs || initial->condition_rhs ||
                    initial->when_true || initial->when_false ||
                    !initial->arguments.empty() ||
                    !initial->argument_names.empty() ||
                    initial->type_arguments_present ||
                    !initial->type_arguments.empty())
                    throw std::runtime_error(
                        "sealed class fields require immutable literal scalar defaults");
            }
        }
    }

    if (program.functions.empty() ||
        program.functions.size() > detail::kMaxRealtimeFunctions)
        throw std::runtime_error("sealed scalar AOT requires a bounded function graph");

    std::unordered_set<std::string> definitions;
    for (const auto& function : program.functions) {
        const auto key = detail::function_key(
            function.name, function.parameters.size());
        if (!definitions.insert(key).second)
            throw std::runtime_error("sealed scalar AOT rejects duplicate function identity");
    }
    if (!definitions.count("main/0"))
        throw std::runtime_error("sealed scalar AOT requires a statically known main/0 entry");

    std::size_t remaining_edges = detail::kMaxRealtimeCallEdges;
    for (const auto& function : program.functions) {
        if (!function.generic_parameters.empty() ||
            !function.method_owner.empty() || function.native_internal ||
            !function.c_export_u32_symbol.empty() ||
            function.scalar_while || function.async_source_i64 ||
            !function.source_tasks.empty() || !function.future_pipelines.empty() ||
            !function.actor_spawns.empty() || !function.actor_sends.empty() ||
            !function.actor_stops.empty() || !function.actor_actions.empty() ||
            !function.actor_field_writes.empty() ||
            !function.ready_await_actor.empty() || !function.done_await_actor.empty() ||
            function.ready_await_form != NativeAwaitForm::None ||
            !function.ready_future_name.empty() ||
            !function.ready_future_actor.empty() ||
            !function.return_class.empty() || !function.return_sum.empty() ||
            function.caught_throw_value ||
            function.return_type == ScalarType::String)
            throw std::runtime_error(
                "sealed scalar AOT rejects unmodeled function, ownership or suspension roots");
        for (const auto& param : function.parameters)
            if (!param.class_name.empty() || !param.sum_type.empty() ||
                param.mode != Parameter::Mode::Value ||
                param.scalar_type == ScalarType::String ||
                !param.generic_type.empty())
                throw std::runtime_error(
                    "sealed scalar AOT rejects non-scalar or borrowed parameter ABI");

        // Every class instance is born as a fresh local stack owner. The
        // shared type/ownership validator has already verified public reads.
        // Reject any captured, borrowed, moved or returned class instance.
        for (const auto& local : function.locals) {
            if (!local.class_name.empty()) {
                if (!allow_class_fields || !admitted_classes.count(local.class_name) ||
                    !local.new_class || local.record_view || local.move_class ||
                    !local.move_source.empty() || local.initializer ||
                    !local.constructor_arguments.empty() || local.heap_owned ||
                    local.int_array || !local.sum_type.empty() ||
                    local.owner_mutable || local.field_mutable)
                    throw std::runtime_error(
                        "sealed class-field AOT requires a fresh stack-only class local");
            } else if (local.new_class || local.record_view || local.move_class ||
                       !local.move_source.empty()) {
                throw std::runtime_error(
                    "sealed class-field AOT rejects unmodeled local ownership");
            }
        }

        // Independently pin each class-field read to a known stack-local
        // owner and public literal class layout. Effect bits alone do not
        // establish field identity: a future expression kind must not get
        // implicit AOT admission merely by reporting StateRead.
        std::unordered_map<std::string, const ClassDecl*> owned_locals;
        std::unordered_set<std::string> local_names;
        for (const auto& local : function.locals) {
            if (!local_names.insert(local.name).second)
                throw std::runtime_error(
                    "sealed AOT rejects duplicate local binding identities");
            if (local.class_name.empty()) continue;
            const auto cls = std::find_if(
                program.classes.begin(), program.classes.end(),
                [&](const ClassDecl& candidate) {
                    return candidate.name == local.class_name;
                });
            if (cls == program.classes.end())
                throw std::runtime_error(
                    "sealed class-field AOT rejects unknown local class layout");
            if (!owned_locals.emplace(local.name, &*cls).second)
                throw std::runtime_error(
                    "sealed class-field AOT rejects duplicate local class ownership");
        }
        const auto verify_expression = [&](const auto& self,
                                          const Expression* expr) -> void {
            if (!expr) return;
            if (expr->kind == Expression::Kind::Field) {
                if (!allow_class_fields || !expr->left ||
                    expr->left->kind != Expression::Kind::Variable)
                    throw std::runtime_error(
                        "sealed class-field AOT rejects unverified field receiver");
                const auto owner = owned_locals.find(expr->left->name);
                if (owner == owned_locals.end())
                    throw std::runtime_error(
                        "sealed class-field AOT rejects nonlocal field receiver");
                const auto& fields = owner->second->fields;
                const auto field = std::find_if(
                    fields.begin(), fields.end(),
                    [&](const ClassField& candidate) {
                        return candidate.name == expr->name;
                    });
                if (field == fields.end() || !field->public_member)
                    throw std::runtime_error(
                        "sealed class-field AOT rejects unregistered or private field");
            }
            self(self, expr->left.get());
            self(self, expr->right.get());
            self(self, expr->condition.get());
            self(self, expr->condition_lhs.get());
            self(self, expr->condition_rhs.get());
            self(self, expr->when_true.get());
            self(self, expr->when_false.get());
            for (const auto& argument : expr->arguments)
                self(self, argument.get());
        };
        const auto verify_statement = [&](const auto& self,
                                         const Statement& statement) -> void {
            // This list is explicit: a new AST statement kind must never
            // inherit admission by relying on a stale effect scanner.
            switch (statement.kind) {
                case Statement::Kind::Call:
                case Statement::Kind::Conditional:
                case Statement::Kind::ScopedLocal:
                case Statement::Kind::Return:
                    break;
                case Statement::Kind::FieldWrite:
                case Statement::Kind::ScalarWrite:
                case Statement::Kind::Defer:
                case Statement::Kind::ArrayCommitFilled:
                case Statement::Kind::ForEach:
                case Statement::Kind::ForIndex:
                default:
                    throw std::runtime_error(
                        "sealed AOT rejects unmodeled or mutating statement kinds");
            }
            if (statement.branch_local) {
                if (owned_locals.count(statement.branch_local->name))
                    throw std::runtime_error(
                        "sealed class-field AOT rejects shadowed class owner");
                if (!statement.branch_local->class_name.empty() ||
                    statement.branch_local->new_class ||
                    statement.branch_local->record_view ||
                    statement.branch_local->move_class ||
                    !statement.branch_local->move_source.empty() ||
                    statement.branch_local->heap_owned ||
                    statement.branch_local->int_array ||
                    !statement.branch_local->sum_type.empty() ||
                    !statement.branch_local->constructor_arguments.empty())
                    throw std::runtime_error(
                        "sealed class-field AOT rejects unverified scoped class owners");
                verify_expression(verify_expression,
                                  statement.branch_local->initializer.get());
                for (const auto& argument :
                     statement.branch_local->constructor_arguments)
                    verify_expression(verify_expression, argument.get());
            }
            verify_expression(verify_expression, statement.expression.get());
            verify_expression(verify_expression, statement.condition.get());
            verify_expression(verify_expression, statement.condition_lhs.get());
            verify_expression(verify_expression, statement.condition_rhs.get());
            verify_expression(verify_expression, statement.loop_step.get());
            verify_expression(verify_expression,
                              statement.array_commit_length.get());
            for (const auto& value : statement.array_commit_values)
                verify_expression(verify_expression, value.get());
            for (const auto& child : statement.then_statements)
                self(self, child);
            for (const auto& child : statement.else_statements)
                self(self, child);
        };
        for (const auto& local : function.locals) {
            verify_expression(verify_expression, local.initializer.get());
            for (const auto& argument : local.constructor_arguments)
                verify_expression(verify_expression, argument.get());
        }
        for (const auto& statement : function.statements)
            verify_statement(verify_statement, statement);
        for (const auto& guard : function.early_returns) {
            verify_expression(verify_expression, guard.condition.get());
            verify_expression(verify_expression, guard.lhs.get());
            verify_expression(verify_expression, guard.rhs.get());
            verify_expression(verify_expression, guard.result.get());
        }
        verify_expression(verify_expression, function.result.get());

        const auto facts = detail::direct_facts(function, remaining_edges, &program);
        // Only typechecked scalar field reads add an admissible effect.
        // Allocation, mutation, unknown bits, IO, suspension and FFI reject.
        const std::uint32_t permitted_effect_bits = allow_class_fields
            ? static_cast<std::uint32_t>(FunctionEffect::StateRead) : 0u;
        if (facts.call_budget_exceeded || !facts.bounded_calls ||
            !facts.bounded_cleanup || facts.unsupported_runtime ||
            facts.unqualified_copy_or_share || facts.unmodeled_expression ||
            !facts.effects.valid() ||
            (facts.effects.bits & ~permitted_effect_bits) != 0u)
            throw std::runtime_error(
                "sealed AOT requires fully modeled statically admitted effects and calls");
        if (facts.calls.size() > remaining_edges)
            throw std::runtime_error("sealed scalar AOT call graph edge budget exceeded");
        remaining_edges -= facts.calls.size();
        for (const auto& callee : facts.calls)
            if (!definitions.count(callee))
                throw std::runtime_error(
                    "sealed scalar AOT rejects unresolved callable: " + callee);
    }
}

static void require_actor_region_v1_aot_graph(const oreslang::Program& program) {
    using namespace oreslang;
    // Reuse the existing fully resolved sealed class-field checker. The new
    // profile narrows it further: class owners may exist only in main/0 and
    // may not cross a function boundary.
    require_sealed_scalar_aot_graph(program, true);
    const auto main = std::find_if(program.functions.begin(), program.functions.end(),
        [](const Function& fn) { return fn.name == "main" && fn.parameters.empty(); });
    if (main == program.functions.end() || main->returns_void ||
        main->return_type != ScalarType::I64)
        throw std::runtime_error("actor-region-v1 requires a synchronous int main/0");
    std::size_t owners = 0;
    for (const auto& fn : program.functions) {
        for (const auto& local : fn.locals) {
            if (local.class_name.empty()) continue;
            if (&fn != &*main)
                throw std::runtime_error(
                    "actor-region-v1 class owners are restricted to main/0 in v1");
            if (!local.new_class || local.record_view || local.move_class ||
                !local.move_source.empty() || local.heap_owned || local.int_array ||
                !local.constructor_arguments.empty() || local.owner_mutable ||
                local.field_mutable)
                throw std::runtime_error(
                    "actor-region-v1 requires fresh immutable class owners");
            ++owners;
        }
    }
    if (!owners || owners > 4096)
        throw std::runtime_error(
            "actor-region-v1 requires between 1 and 4096 lexical class owners");
    for (const auto& cls : program.classes) {
        const std::size_t bytes = cls.fields.size() * sizeof(std::int64_t);
        if (!bytes || bytes > 1024u * 1024u)
            throw std::runtime_error("actor-region-v1 class layout exceeds region slot bounds");
        for (const auto& field : cls.fields)
            if (!field.final_field || field.mutable_referent || field.int_array ||
                !field.class_name.empty() || !field.sum_type.empty() ||
                field.scalar_type == ScalarType::String)
                throw std::runtime_error(
                    "actor-region-v1 supports only immutable scalar class fields");
    }
}

int main(int argc, char** argv) {
    bool experimental_owned_int = false;
    bool experimental_rt_scalar_loop = false;
    bool wasm32_target = false;
    bool cortex_m_target = false;
    bool target_seen = false;
    bool native_hard_rt_profile = false;
    bool profile_seen = false;
    std::optional<oreslang::frontend::LibraryProfile> library_profile;
    bool library_profile_seen = false;
    bool allocator_provider_seen = false;
    bool bounded_allocator_provider = false;
    bool rt_check_only = false;
    bool rt_entry_seen = false;
    bool rt_report_seen = false;
    bool constraint_descriptors_only = false;
    std::string rt_entry = "main/0";
    std::string rt_report_path;
    bool tree_shake = false;
    bool sealed_scalar_aot = false;
    bool sealed_class_fields_aot = false;
    bool actor_region_v1_aot = false;
    bool isolate_entry_seen = false;
    std::string isolate_entry;
    std::vector<std::string> tree_shake_roots;
    const char* path = nullptr;
    // Host-only severity policy: last CLI flag overrides the environment.
    const char* envLevel = std::getenv("ORES_CORE_LOG_LEVEL");
    std::string coreLogLevel = envLevel ? envLevel : "off";
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (arg.rfind("--allocator-provider=", 0) == 0) {
            if (allocator_provider_seen) {
                std::cerr << "duplicate allocator-provider option\n";
                return 2;
            }
            allocator_provider_seen = true;
            const std::string provider =
                arg.substr(std::string_view("--allocator-provider=").size());
            if (provider != "bounded-arena") {
                std::cerr << "unsupported allocator provider: " << provider
                          << " (expected bounded-arena)\n";
                return 2;
            }
            bounded_allocator_provider = true;
        } else if (arg.rfind("--library-profile=", 0) == 0) {
            if (library_profile_seen) {
                std::cerr << "duplicate library-profile option\n";
                return 2;
            }
            library_profile_seen = true;
            const std::string profile =
                arg.substr(std::string_view("--library-profile=").size());
            library_profile = oreslang::frontend::parse_library_profile(profile);
            if (!library_profile.has_value()) {
                std::cerr << "unsupported library profile: " << profile
                          << " (expected core or std)\n";
                return 2;
            }
        } else if (arg.rfind("--aot-profile=", 0) == 0) {
            if (sealed_scalar_aot || sealed_class_fields_aot || actor_region_v1_aot) {
                std::cerr << "duplicate AOT profile option\n"; return 2;
            }
            const std::string profile = arg.substr(std::string_view("--aot-profile=").size());
            if (profile == "sealed-scalar-v1") {
                sealed_scalar_aot = true;
            } else if (profile == "sealed-class-fields-v1") {
                sealed_class_fields_aot = true;
            } else if (profile == "actor-region-v1") {
                actor_region_v1_aot = true;
            } else {
                std::cerr << "unsupported AOT profile (expected sealed-scalar-v1, sealed-class-fields-v1 or actor-region-v1)\n";
                return 2;
            }
        } else if (arg.rfind("--profile=", 0) == 0) {
            if (profile_seen) { std::cerr << "duplicate profile option\n"; return 2; }
            profile_seen = true;
            const std::string profile = arg.substr(std::string_view("--profile=").size());
            if (profile != "native-hard-rt") {
                std::cerr << "unsupported native compiler profile: " << profile << '\n';
                return 2;
            }
            native_hard_rt_profile = true;
        } else if (arg == "--rt-check") {
            if (rt_check_only) { std::cerr << "duplicate rt-check option\n"; return 2; }
            rt_check_only = true;
            native_hard_rt_profile = true;
        } else if (arg.rfind("--rt-entry=", 0) == 0) {
            if (rt_entry_seen) { std::cerr << "duplicate rt-entry option\n"; return 2; }
            rt_entry_seen = true;
            rt_entry = arg.substr(std::string_view("--rt-entry=").size());
            if (rt_entry.empty()) { std::cerr << "rt-entry cannot be empty\n"; return 2; }
        } else if (arg.rfind("--rt-report=", 0) == 0) {
            if (rt_report_seen) { std::cerr << "duplicate rt-report option\n"; return 2; }
            rt_report_seen = true;
            rt_report_path = arg.substr(std::string_view("--rt-report=").size());
            if (rt_report_path.empty()) { std::cerr << "rt-report path cannot be empty\n"; return 2; }
        } else if (arg == "--constraint-descriptors") {
            if (constraint_descriptors_only) {
                std::cerr << "duplicate constraint-descriptors option\n";
                return 2;
            }
            constraint_descriptors_only = true;
        } else if (arg.rfind("--core-log-level=", 0) == 0) {
            coreLogLevel = arg.substr(std::string_view("--core-log-level=").size());
        } else if (arg == "--no-core-log") {
            coreLogLevel = "off";
        } else if (arg == "--experimental-rt-scalar-loop") {
            if (experimental_rt_scalar_loop) {
                std::cerr << "duplicate rt scalar loop option\n"; return 2;
            }
            experimental_rt_scalar_loop = true;
        } else if (arg == "--experimental-owned-int") {
            if (experimental_owned_int) { std::cerr << "duplicate owned-int option\n"; return 2; }
            experimental_owned_int = true;
        } else if (arg.rfind("--isolate-entry=", 0) == 0) {
            if (isolate_entry_seen) { std::cerr << "duplicate isolate entry\n"; return 2; }
            isolate_entry_seen = true;
            isolate_entry = arg.substr(std::string("--isolate-entry=").size());
            if (isolate_entry.empty()) { std::cerr << "empty isolate entry\n"; return 2; }
        } else if (arg == "--tree-shake") {
            if (tree_shake) { std::cerr << "duplicate tree-shake option\n"; return 2; }
            tree_shake = true;
        } else if (arg.rfind("--tree-shake-root=", 0) == 0) {
            const std::string root = arg.substr(18);
            if (root.empty()) { std::cerr << "empty tree-shake root\n"; return 2; }
            tree_shake_roots.push_back(root);
        } else if (arg.rfind("--target=", 0) == 0) {
            if (target_seen) { std::cerr << "duplicate target option\n"; return 2; }
            target_seen = true;
            const std::string target = arg.substr(9);
            if (target == "host") {
                // Historical default: host triple and ABI remain inferred downstream.
            } else if (target == "thumbv7m-none-eabi") {
                cortex_m_target = oreslang::TargetAbi::parse(target).experimental_cortexm_ir();
            } else if (target == "wasm32" || target == "wasm32-unknown-unknown") {
                wasm32_target = oreslang::TargetAbi::parse("wasm32-unknown-unknown").experimental_wasm32_ir();
            } else {
                try {
                    (void)oreslang::TargetAbi::parse(target);
                    std::cerr << "target has no certified IR/ABI lowering: " << target << '\n';
                } catch (const std::invalid_argument&) {
                    std::cerr << "unknown target triple: " << target << '\n';
                }
                return 2;
            }
        } else if (arg.rfind("--cpu=", 0) == 0) {
            std::cerr << "explicit CPU feature selection not implemented: " << arg << '\n';
            return 2;
        } else if (!arg.empty() && arg[0] != '-' && path == nullptr) path = argv[i];
        else { std::cerr << "unrecognized or duplicate option: " << arg << '\n'; return 2; }
    }
    // Public library architecture has exactly two layers. For now the native
    // backend only certifies the freestanding core profile on targets whose ABI
    // is explicitly no-OS, and the std profile on the ordinary hosted target.
    // This is intentionally fail-closed until effect/capability closure can
    // prove additional combinations.
    const bool freestanding_library_target = wasm32_target || cortex_m_target;
    if (!library_profile.has_value()) {
        library_profile =
            oreslang::frontend::default_library_profile(freestanding_library_target);
    }
    const std::string_view library_profile_error =
        oreslang::frontend::library_profile_admission_error(
            *library_profile, freestanding_library_target);
    if (!library_profile_error.empty()) {
        std::cerr << library_profile_error;
        if (*library_profile == oreslang::frontend::LibraryProfile::Core) {
            std::cerr << " (wasm32-unknown-unknown or thumbv7m-none-eabi)";
        }
        std::cerr << "\n";
        return 2;
    }
    if (bounded_allocator_provider && !freestanding_library_target) {
        std::cerr << "bounded-arena allocator provider is certified only for freestanding targets\n";
        return 2;
    }
    if (bounded_allocator_provider &&
            *library_profile != oreslang::frontend::LibraryProfile::Core) {
        std::cerr << "allocator provider requires library-profile=core\n";
        return 2;
    }
    const std::uint32_t admitted_provider_bits =
        bounded_allocator_provider
            ? oreslang::frontend::core_provider_bit(
                oreslang::frontend::CoreProviderRequirement::Allocator)
            : 0u;

    if ((sealed_scalar_aot || sealed_class_fields_aot || actor_region_v1_aot) &&
        (wasm32_target || cortex_m_target || experimental_owned_int ||
         experimental_rt_scalar_loop || native_hard_rt_profile || rt_check_only ||
         rt_entry_seen || rt_report_seen || tree_shake ||
         !tree_shake_roots.empty() || isolate_entry_seen ||
         bounded_allocator_provider || constraint_descriptors_only)) {
        std::cerr << "sealed scalar AOT only supports isolated host scalar codegen\n";
        return 2;
    }
    if (constraint_descriptors_only &&
            (tree_shake || !tree_shake_roots.empty() || native_hard_rt_profile ||
             rt_check_only || rt_entry_seen || rt_report_seen ||
             experimental_owned_int || experimental_rt_scalar_loop ||
             target_seen || allocator_provider_seen)) {
        std::cerr << "constraint-descriptors is a metadata-only mode and cannot be combined with execution/codegen options\n";
        return 2;
    }
    if (isolate_entry_seen && (tree_shake || !tree_shake_roots.empty() ||
            wasm32_target || cortex_m_target || experimental_owned_int ||
            experimental_rt_scalar_loop || native_hard_rt_profile ||
            bounded_allocator_provider || rt_check_only || rt_report_seen)) {
        std::cerr << "isolate entry supports only ordinary host scalar codegen\n";
        return 2;
    }
    if (!tree_shake && !tree_shake_roots.empty()) {
        std::cerr << "tree-shake-root requires --tree-shake\n";
        return 2;
    }
    if (rt_entry_seen && !native_hard_rt_profile && !rt_report_seen) {
        std::cerr << "rt-entry requires --rt-check, --profile=native-hard-rt, or --rt-report\n";
        return 2;
    }
    oreslang::CoreLog::Level admittedLevel = oreslang::CoreLog::Level::Off;
    if (!oreslang::CoreLog::parse_level(coreLogLevel, admittedLevel)) {
        std::cerr << "invalid core log level (off|error|warn|info|debug|trace)\n";
        return 2;
    }
    // Default/off does not allocate or initialize 8192 recorder slots.
    // Optional diagnostics must not make ordinary compilation fail on OOM.
    std::unique_ptr<oreslang::CoreLog> coreLog;
    if (admittedLevel != oreslang::CoreLog::Level::Off)
        coreLog.reset(new (std::nothrow) oreslang::CoreLog(admittedLevel));
    if (path == nullptr) {
        std::cerr << "usage: oreslang-llvmc [--allocator-provider=bounded-arena] [--library-profile=core|std] [--constraint-descriptors] [--aot-profile=sealed-scalar-v1|sealed-class-fields-v1|actor-region-v1] [--tree-shake] [--tree-shake-root=name ...] [--profile=native-hard-rt] [--rt-check] [--rt-entry=name/arity] [--rt-report=path] [--target=host|wasm32|wasm32-unknown-unknown|thumbv7m-none-eabi] [--experimental-owned-int] [--experimental-rt-scalar-loop] [--core-log-level=off|error|warn|info|debug|trace] [--no-core-log] <source.atto|source.ores>\n"
                  << "Supported: int functions/parameters, return arithmetic, and same-module calls.\n";
        return 2;
    }
    std::string source_module;
    std::string source_namespace;
    try {
        std::cout.exceptions(std::ios::badbit | std::ios::failbit);
        const std::size_t kMaxSourceBytes = cortex_m_target ? 128u * 1024u : 8u * 1024u * 1024u;
        const char* source_overflow = cortex_m_target
            ? "cortex-m source exceeds 128 KiB limit"
            : "native source exceeds 8 MiB admission limit";
        const SourceIdentity expected_source_identity =
            capture_source_identity(path, kMaxSourceBytes, source_overflow);
        SourceIdentity source_identity;
        const auto text = read_regular_source(
            path, kMaxSourceBytes, source_overflow,
            &source_identity, &expected_source_identity);
        require_valid_source_utf8(text);
        // Target/capability directives are lexical comments in the native
        // subset. Reject rather than silently discard unverified portability
        // constraints before parsing or emitting LLVM IR.
        if (text.find("// ores:") != std::string::npos) {
            throw std::runtime_error("native target admission unsupported: ores environment headers require compiler verification");
        }
        if (experimental_rt_scalar_loop && (wasm32_target || experimental_owned_int))
            throw std::runtime_error("scalar-loop frame requires native host and no owned-int experiment");
        if (cortex_m_target && text.size() > 128u * 1024u)
            throw std::runtime_error("cortex-m source exceeds 128 KiB limit");
        if (native_hard_rt_profile && (wasm32_target || cortex_m_target))
            throw std::runtime_error("native-hard-rt profile currently requires the host target");
        if (native_hard_rt_profile && experimental_owned_int)
            throw std::runtime_error("native-hard-rt profile rejects experimental owned heap lowering");

        auto ast = oreslang::load_native_program(
            path, text, source_identity, experimental_owned_int,
            experimental_rt_scalar_loop, !wasm32_target && !cortex_m_target,
            admitted_provider_bits);
        source_module = ast.module;
        source_namespace = ast.namespace_name;

        // Strict admission happens on the fully resolved and ownership-checked
        // Program, before any executable LLVM IR or sidecar is emitted.
        if (sealed_scalar_aot) require_sealed_scalar_aot_graph(ast);
        if (sealed_class_fields_aot) require_sealed_scalar_aot_graph(ast, true);
        if (actor_region_v1_aot) require_actor_region_v1_aot_graph(ast);

        if (constraint_descriptors_only) {
            for (const auto& descriptor : ast.type_constraint_descriptors) {
                std::cout << "descriptor\t"
                          << descriptor.schema_id << '\t'
                          << descriptor.qualified_type_name << '\t'
                          << descriptor.representation << '\t'
                          << (descriptor.validation_required ? "true" : "false") << '\t'
                          << descriptor.fingerprint << '\t'
                          << descriptor.canonical << '\n';
            }
            std::cout.flush();
            return 0;
        }

        if (rt_report_seen && path != nullptr && paths_alias(path, rt_report_path))
            throw std::runtime_error("rt-report path must not overwrite source input");

        if (native_hard_rt_profile || rt_report_seen) {
            const auto report = oreslang::analyze_native_hard_rt(ast, rt_entry);
            const std::string report_json = report.to_json();
            if (rt_report_seen) {
                std::ofstream output(rt_report_path, std::ios::out | std::ios::trunc);
                if (!output) throw std::runtime_error("cannot open rt-report output");
                output.exceptions(std::ios::badbit | std::ios::failbit);
                output << report_json;
                output.flush();
            }
            if (rt_check_only) {
                std::cout << report_json;
                std::cout.flush();
                if (coreLog) coreLog->dump(stderr);
                return report.source_admitted ? 0 : 1;
            }
            if (native_hard_rt_profile && !report.source_admitted) {
                throw std::runtime_error(
                    "native-hard-rt admission failed [" + report.diagnostic +
                    "]: " + report.reason);
            }
        }

        if (cortex_m_target) {
            // Deliberately small freestanding ABI: i64 scalar functions only.
            // Never lower native actors, heap regions or unverified host calls
            // merely because they happen to compile into some LLVM IR.
            if (experimental_owned_int || experimental_rt_scalar_loop ||
                !ast.module.empty() || !ast.classes.empty() ||
                !ast.cooperative_loops.empty() || ast.functions.empty())
                throw std::runtime_error("cortex-m target supports only freestanding scalar functions");
            constexpr std::size_t max_functions = 64;
            constexpr std::size_t max_parameters = 8;
            constexpr std::size_t max_expression_nodes = 4096;
            constexpr std::size_t max_expression_depth = 32;
            constexpr std::size_t max_call_depth = 16;
            if (ast.functions.size() > max_functions)
                throw std::runtime_error("cortex-m function count exceeds 64");
            std::size_t expression_nodes = 0;
            std::unordered_map<std::string, std::vector<std::string>> edges;
            std::function<bool(const oreslang::Expression&, std::size_t,
                               std::vector<std::string>&)> scalar =
                [&](const oreslang::Expression& expr, std::size_t depth,
                    std::vector<std::string>& calls) -> bool {
                if (depth > max_expression_depth || ++expression_nodes > max_expression_nodes)
                    throw std::runtime_error("cortex-m expression budget exceeded");

                using Kind = oreslang::Expression::Kind;
                switch (expr.kind) {
                    case Kind::Integer: case Kind::Boolean:
                    case Kind::Variable: return true;
                    case Kind::Negate: case Kind::LogicalNot:
                        return expr.left && scalar(*expr.left, depth + 1, calls);
                    case Kind::Add: case Kind::Subtract: case Kind::Multiply:
                    case Kind::Equal: case Kind::NotEqual:
                    case Kind::Less: case Kind::LessEqual:
                    case Kind::Greater: case Kind::GreaterEqual:
                    case Kind::LogicalAnd: case Kind::LogicalOr:
                    case Kind::LogicalXor:
                        return expr.left && expr.right &&
                               scalar(*expr.left, depth + 1, calls) &&
                               scalar(*expr.right, depth + 1, calls);
                    case Kind::Call:
                        if (expr.arguments.size() > max_parameters) return false;
                        calls.push_back(expr.name);
                        for (const auto& arg : expr.arguments)
                            if (!arg || !scalar(*arg, depth + 1, calls)) return false;
                        return true;
                    case Kind::ArrayCall:
                        if (!bounded_allocator_provider || !expr.left ||
                                expr.left->kind != Kind::Variable ||
                                expr.name != "get" ||
                                expr.arguments.size() != 1 ||
                                !expr.arguments.front())
                            return false;
                        return scalar(
                            *expr.arguments.front(), depth + 1, calls);
                    case Kind::Field:
                        return bounded_allocator_provider && expr.left &&
                            expr.left->kind == Kind::Variable &&
                            expr.name == "size";
                    default: return false;
                }
            };
            if (!ast.actors.empty()) {
                if (ast.actors.size() > 8 || ast.functions.size() != 1 ||
                    ast.functions.front().name != "main")
                    throw std::runtime_error("cortex-m actors require 1..8 actor types and only main");
                const auto& start = ast.functions.front();
                if (start.returns_void || !start.result ||
                    start.result->kind != oreslang::Expression::Kind::Integer ||
                    !start.locals.empty() || !start.statements.empty() ||
                    !start.early_returns.empty() || start.has_declared_effects ||
                    start.actor_spawns.empty() || start.actor_spawns.size() > 8 ||
                    start.actor_sends.size() > 32 ||
                    start.actor_stops.size() != start.actor_spawns.size() ||
                    !start.parameters.empty())
                    throw std::runtime_error("cortex-m actor bootstrap requires bounded source main");
                if (start.actor_actions.size() !=
                        start.actor_sends.size() + start.actor_stops.size())
                    throw std::runtime_error("cortex-m actor action plan inconsistent");
                for (const auto& actor : ast.actors) {
                    if (actor.fields.empty() || actor.fields.size() > 8 ||
                        actor.on_message.actor_field_writes.size() > 16 ||
                        !actor.on_message.result)
                        throw std::runtime_error("cortex-m actor field or handler budget exceeded");
                    for (const auto& field : actor.fields) {
                        if (!field.initializer ||
                            field.initializer->kind != oreslang::Expression::Kind::Integer)
                            throw std::runtime_error("cortex-m actors need literal i64 field initialization");
                    }
                    std::vector<std::string> calls;
                    if (!scalar(*actor.on_message.result, 0, calls) || !calls.empty())
                        throw std::runtime_error("cortex-m actor return must be scalar without guest calls");
                    for (const auto& write : actor.on_message.actor_field_writes) {
                        calls.clear();
                        if (!write.expression || !scalar(*write.expression, 0, calls) ||
                            !calls.empty())
                            throw std::runtime_error("cortex-m actor writes must be scalar without guest calls");
                    }
                }
                for (const auto& send : start.actor_sends) {
                    if (!send.payload ||
                        send.payload->kind != oreslang::Expression::Kind::Integer)
                        throw std::runtime_error("cortex-m actor send must be a literal i64");
                }
                // All bootstrap effects are statically ordered. Prove no
                // mailbox can overflow before lowering irreversible void sends.
                // The runtime's 16-entry cap is an ABI contract, not a hint.
                std::unordered_map<std::string, std::size_t> pending;
                std::unordered_set<std::string> stopped;
                for (const auto& spawn : start.actor_spawns) {
                    if (!pending.emplace(spawn.name, 0).second)
                        throw std::runtime_error("cortex-m duplicate actor ref");
                }
                for (const auto& action : start.actor_actions) {
                    std::string ref;
                    if (action.kind == oreslang::ActorAction::Kind::Send) {
                        if (action.index >= start.actor_sends.size())
                            throw std::runtime_error("cortex-m invalid actor send index");
                        ref = start.actor_sends[action.index].reference;
                    } else {
                        if (action.index >= start.actor_stops.size())
                            throw std::runtime_error("cortex-m invalid actor stop index");
                        ref = start.actor_stops[action.index];
                    }
                    const auto it = pending.find(ref);
                    if (it == pending.end() || stopped.count(ref))
                        throw std::runtime_error("cortex-m invalid or stopped actor capability");
                    if (action.kind == oreslang::ActorAction::Kind::Send) {
                        if (++it->second > 16)
                            throw std::runtime_error("cortex-m static mailbox capacity exceeded (16)");
                    } else {
                        stopped.insert(ref);
                    }
                }
                if (stopped.size() != pending.size())
                    throw std::runtime_error("cortex-m bootstrap must stop all actors");
                // The trusted C runtime owns static actors, mailboxes, and
                // orderly stop/drain; this does not admit guest awaits.
            }
            auto guarded_scalar_returns = [&](const oreslang::Function& fn) {
                if (fn.statements.size() > 8) return false;
                for (const auto& statement : fn.statements) {
                    if (statement.kind != oreslang::Statement::Kind::Conditional ||
                            !statement.condition ||
                            statement.condition_lhs || statement.condition_rhs ||
                            !statement.else_statements.empty() ||
                            statement.then_statements.size() != 1)
                        return false;
                    const auto& terminal = statement.then_statements.front();
                    if (terminal.kind != oreslang::Statement::Kind::Return ||
                            !terminal.expression ||
                            terminal.condition || terminal.condition_lhs ||
                            terminal.condition_rhs ||
                            !terminal.then_statements.empty() ||
                            !terminal.else_statements.empty())
                        return false;
                    std::vector<std::string> condition_calls;
                    if (!scalar(*statement.condition, 0, condition_calls) ||
                            !condition_calls.empty())
                        return false;
                    std::vector<std::string> return_calls;
                    if (!scalar(*terminal.expression, 0, return_calls))
                        return false;
                }
                return true;
            };
            auto bounded_allocator_function =
                [&](const oreslang::Function& fn) {
                    if (!bounded_allocator_provider ||
                            fn.locals.empty() || fn.locals.size() > 20 ||
                            !fn.statements.empty())
                        return false;
                    std::unordered_set<std::string> arrays;
                    std::size_t owners = 0;
                    std::size_t effects = 0;
                    for (const auto& local : fn.locals) {
                        if (local.int_array) {
                            if (++owners > 4 || !local.owner_mutable ||
                                    local.heap_owned || local.new_class ||
                                    !local.class_name.empty() ||
                                    local.initializer)
                                return false;
                            if (!arrays.insert(local.name).second)
                                return false;
                            continue;
                        }
                        if (local.name.rfind("$effect", 0) != 0 ||
                                !local.initializer ||
                                local.initializer->kind !=
                                    oreslang::Expression::Kind::ArrayCall ||
                                ++effects > 16)
                            return false;
                        const auto& call = *local.initializer;
                        if (!call.left ||
                                call.left->kind !=
                                    oreslang::Expression::Kind::Variable ||
                                !arrays.count(call.left->name))
                            return false;
                        const std::size_t expected =
                            call.name == "add" ? 1 :
                            (call.name == "set" ? 2 : 0);
                        if (expected == 0 ||
                                call.arguments.size() != expected)
                            return false;
                        for (const auto& argument : call.arguments) {
                            std::vector<std::string> calls;
                            if (!argument ||
                                    !scalar(*argument, 0, calls) ||
                                    !calls.empty())
                                return false;
                        }
                    }
                    return owners != 0;
                };
            for (const auto& fn : ast.functions) {
                const bool scalar_function =
                    fn.locals.empty() && guarded_scalar_returns(fn);
                const bool allocator_function =
                    bounded_allocator_function(fn);
                if ((!ast.actors.empty() && fn.name != "main") ||
                    (ast.actors.empty() && fn.name == "main") ||
                    fn.returns_void || !fn.result ||
                    (!scalar_function && !allocator_function) ||
                    !fn.early_returns.empty() ||
                    (ast.actors.empty() && (!fn.actor_spawns.empty() ||
                        !fn.actor_sends.empty() || !fn.actor_stops.empty() ||
                        !fn.actor_actions.empty() || !fn.actor_field_writes.empty())) ||
                    fn.scalar_while || fn.caught_throw_value ||
                    !fn.caught_throw_defers.empty() || fn.has_declared_effects)
                    throw std::runtime_error(
                        "cortex-m target rejects main, effects or unbounded functions");
                if (fn.parameters.size() > max_parameters)
                    throw std::runtime_error("cortex-m parameter budget exceeded");
                for (const auto& param : fn.parameters)
                    if (!param.class_name.empty() ||
                        param.mode != oreslang::Parameter::Mode::Value)
                        throw std::runtime_error("cortex-m target requires value int parameters");
                std::vector<std::string> calls;
                if (!scalar(*fn.result, 0, calls))
                    throw std::runtime_error("cortex-m target requires bounded scalar expressions");
                if (!edges.emplace(fn.name, std::move(calls)).second)
                    throw std::runtime_error("cortex-m target rejects overloaded function names");
            }
            // Source-level cycles are forbidden on tiny stacks. The call-graph
            // path limit caps nesting even when individual ASTs are shallow.
            std::unordered_set<std::string> active;
            std::unordered_map<std::string, std::size_t> longest;
            std::function<std::size_t(const std::string&)> walk =
                [&](const std::string& name) -> std::size_t {
                const auto known = longest.find(name);
                if (known != longest.end()) return known->second;
                if (!active.insert(name).second)
                    throw std::runtime_error("cortex-m recursive call graph rejected");
                auto node = edges.find(name);
                if (node == edges.end())
                    throw std::runtime_error("cortex-m unresolved function call");
                std::size_t depth = 1;
                for (const auto& dependency : node->second) {
                    const auto child = walk(dependency);
                    if (child >= max_call_depth)
                        throw std::runtime_error("cortex-m call depth budget exceeded");
                    depth = std::max(depth, child + 1);
                }
                active.erase(name);
                longest.emplace(name, depth);
                return depth;
            };
            for (const auto& fn : ast.functions) (void)walk(fn.name);
        }
        if (tree_shake) {
            // Tree shaking is an optimization, not conditional compilation.
            // Validate/codegen the complete admitted source once before pruning
            // so unsupported target features cannot hide in unreachable code.
            (void)oreslang::emit_verified_llvm_ir(
                ast, wasm32_target, cortex_m_target,
                bounded_allocator_provider, {}, actor_region_v1_aot);

            // The shipped pass is deliberately function-only. Richer Program
            // owners have hidden semantic/ABI roots that are not represented
            // by the direct-call graph yet. Fail closed rather than deleting
            // a helper that is reachable only from an actor handler, class
            // initializer, or host-driven continuation.
            require_function_only_tree_shake_surface(ast);
            tree_shake_functions(ast, tree_shake_roots);
        }
        std::cout << oreslang::emit_verified_llvm_ir(
                ast, wasm32_target, cortex_m_target,
                bounded_allocator_provider, isolate_entry, actor_region_v1_aot);
        std::cout.flush();
        if (coreLog) coreLog->dump(stderr);
        return 0;
    } catch (const std::exception& error) {
        // No guest payload, exception message or source path enters CoreLog.
        if (coreLog)
            coreLog->record(oreslang::CoreLog::Level::Error,
                            oreslang::CoreLog::EventId::CompileFailed, 1);
        std::cerr << "oreslang-llvmc: ";
        std::string diagnostic = error.what();
        if (path != nullptr && *path != '\0') {
            const std::string source_path(path);
            // Full host paths are private CI metadata, not error context.
            // Replace exact input-path occurrences before rendering.
            std::size_t found = 0;
            while ((found = diagnostic.find(source_path, found)) != std::string::npos) {
                diagnostic.replace(found, source_path.size(), "<source>");
                found += sizeof("<source>") - 1;
            }
        }
        print_safe_diagnostic(std::cerr, diagnostic, 2048);
        if (path != nullptr) {
            std::string_view source_name(path);
            const auto slash = source_name.find_last_of("/\\");
            if (slash != std::string_view::npos) source_name.remove_prefix(slash + 1);
            std::cerr << " [file: ";
            print_safe_diagnostic(std::cerr, source_name, 128);
            std::cerr << "]";
        }
        if (!source_module.empty()) {
            std::cerr << " [module: ";
            print_safe_diagnostic(std::cerr, source_module, 128);
            std::cerr << "]";
        }
        if (!source_namespace.empty()) {
            std::cerr << " [namespace: ";
            print_safe_diagnostic(std::cerr, source_namespace, 128);
            std::cerr << "]";
        }
        std::cerr << '\n';
        if (coreLog) coreLog->dump(stderr);
        return 1;
    }
}
