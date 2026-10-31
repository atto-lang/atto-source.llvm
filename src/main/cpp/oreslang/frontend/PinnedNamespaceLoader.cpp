#include "oreslang/frontend/PinnedNamespaceLoader.hpp"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace oreslang {
namespace {

constexpr std::size_t kMaxPinnedNamespaceSources = 128;
constexpr std::size_t kMaxPinnedNamespaceBytes = 8u * 1024u * 1024u;
constexpr std::size_t kMaxPinnedIdentityBytes = 1024;

struct PinnedUnit {
    std::string source_unit_id;
    std::string package_instance_id;
    Program program;
};

struct CallableId {
    std::string source_unit_id;
    std::string name;
    std::size_t arity = 0;

    bool operator<(const CallableId& other) const {
        return std::tie(source_unit_id, name, arity)
            < std::tie(other.source_unit_id, other.name, other.arity);
    }
};

[[noreturn]] void fail(const std::string& reason) {
    throw std::runtime_error("pinned namespace lowering: " + reason);
}

bool valid_host_identity(const std::string& value) {
    if (value.empty() || value.size() > kMaxPinnedIdentityBytes) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return ch >= 32 && ch != 127;
    });
}

void require_entry_scalar_surface(const Program& program) {
    if (!program.namespace_name.empty())
        fail("entry source cannot also contribute a namespace");
    if (!program.module.empty())
        fail("entry source module wrapper is not admitted in namespace v1");
    if (!program.classes.empty() || !program.actors.empty()
            || !program.contracts.empty() || !program.conforms.empty()
            || !program.type_only_declarations.empty()
            || !program.type_constraint_descriptors.empty()
            || !program.sum_types.empty() || !program.generators.empty()
            || !program.cooperative_loops.empty())
        fail("entry source requires scalar function-only lowering");
    for (const auto& fn : program.functions) {
        if (fn.name == "init")
            fail("namespace entry init requires once-only lifecycle lowering");
        if (!fn.statements.empty() || !fn.early_returns.empty()
                || fn.caught_throw_value || !fn.catch_binding.empty()
                || !fn.caught_throw_defers.empty() || fn.async_source_i64
                || !fn.source_tasks.empty() || !fn.future_pipelines.empty()
                || !fn.actor_spawns.empty() || !fn.actor_sends.empty()
                || !fn.actor_stops.empty() || !fn.actor_actions.empty()
                || !fn.actor_field_writes.empty() || fn.scalar_while)
            fail("entry source requires direct scalar expression functions");
    }
}

void require_entry_namespace_bindings_unshadowed(const Program& program) {
    std::set<std::string> bindings;
    for (const auto& imported : program.imports) {
        if (!imported.selected_namespace) continue;
        bindings.insert(imported.local_name);
    }
    for (const auto& fn : program.functions) {
        if (bindings.count(fn.name))
            fail("entry function shadows namespace binding '" + fn.name + "'");
        for (const auto& parameter : fn.parameters) {
            if (bindings.count(parameter.name))
                fail("entry parameter shadows namespace binding '" + parameter.name + "'");
        }
        for (const auto& local : fn.locals) {
            if (bindings.count(local.name))
                fail("entry local shadows namespace binding '" + local.name + "'");
        }
    }
}

const frontend::PackageNamespaceExport& resolve_export(
        const frontend::PackageNamespaceGraph& graph,
        const frontend::PackageNamespaceKey& target,
        const std::string& member,
        std::size_t arity) {
    const auto group = graph.namespaces.find(target);
    if (group == graph.namespaces.end())
        fail("resolved namespace target disappeared from verified graph");
    const frontend::PackageNamespaceExport* selected = nullptr;
    for (const auto& exported : group->second.exports) {
        if (exported.name != member || exported.arity != arity) continue;
        if (selected)
            fail("ambiguous namespace callable after graph validation");
        selected = &exported;
    }
    if (!selected)
        fail("unknown namespace member or wrong arity '" + member + "'");
    return *selected;
}

void rewrite_entry_expression(
        const std::string& source_unit_id,
        Expression* expression,
        const frontend::PackageNamespaceGraph& graph,
        const std::map<std::string, std::string>& prefixes) {
    if (!expression) return;
    rewrite_entry_expression(source_unit_id, expression->left.get(), graph, prefixes);
    rewrite_entry_expression(source_unit_id, expression->right.get(), graph, prefixes);
    rewrite_entry_expression(source_unit_id, expression->condition.get(), graph, prefixes);
    rewrite_entry_expression(source_unit_id, expression->condition_lhs.get(), graph, prefixes);
    rewrite_entry_expression(source_unit_id, expression->condition_rhs.get(), graph, prefixes);
    rewrite_entry_expression(source_unit_id, expression->when_true.get(), graph, prefixes);
    rewrite_entry_expression(source_unit_id, expression->when_false.get(), graph, prefixes);
    for (auto& argument : expression->arguments)
        rewrite_entry_expression(source_unit_id, argument.get(), graph, prefixes);

    if (expression->kind != Expression::Kind::Call) return;
    const auto dot = expression->name.find('.');
    if (dot == std::string::npos) return;
    if (dot == 0 || dot + 1 >= expression->name.size()
            || expression->name.find('.', dot + 1) != std::string::npos)
        fail("namespace call must be exactly binding.member");

    const std::string binding = expression->name.substr(0, dot);
    const std::string member = expression->name.substr(dot + 1);
    const auto imported = graph.imports.find({source_unit_id, binding});
    if (imported == graph.imports.end())
        fail("unknown pinned namespace binding '" + binding + "'");
    const auto& selected = resolve_export(
        graph, imported->second, member, expression->arguments.size());
    const auto prefix = prefixes.find(selected.source_unit_id);
    if (prefix == prefixes.end())
        fail("namespace export source is not in the reachable pinned inventory");
    expression->name = prefix->second + member;
}

void rewrite_entry_functions(
        const std::string& source_unit_id,
        Program& program,
        const frontend::PackageNamespaceGraph& graph,
        const std::map<std::string, std::string>& prefixes) {
    for (auto& fn : program.functions) {
        for (auto& local : fn.locals)
            rewrite_entry_expression(
                source_unit_id, local.initializer.get(), graph, prefixes);
        rewrite_entry_expression(source_unit_id, fn.result.get(), graph, prefixes);
    }
}

void rewrite_provider_expression(
        const CallableId& caller,
        const frontend::PackageNamespaceKey& namespace_key,
        Expression* expression,
        const frontend::PackageNamespaceGraph& graph,
        const std::map<std::string, std::string>& prefixes,
        std::map<CallableId, std::set<CallableId>>& edges) {
    if (!expression) return;
    rewrite_provider_expression(
        caller, namespace_key, expression->left.get(), graph, prefixes, edges);
    rewrite_provider_expression(
        caller, namespace_key, expression->right.get(), graph, prefixes, edges);
    rewrite_provider_expression(
        caller, namespace_key, expression->condition.get(), graph, prefixes, edges);
    rewrite_provider_expression(
        caller, namespace_key, expression->condition_lhs.get(), graph, prefixes, edges);
    rewrite_provider_expression(
        caller, namespace_key, expression->condition_rhs.get(), graph, prefixes, edges);
    rewrite_provider_expression(
        caller, namespace_key, expression->when_true.get(), graph, prefixes, edges);
    rewrite_provider_expression(
        caller, namespace_key, expression->when_false.get(), graph, prefixes, edges);
    for (auto& argument : expression->arguments)
        rewrite_provider_expression(
            caller, namespace_key, argument.get(), graph, prefixes, edges);

    if (expression->kind != Expression::Kind::Call) return;
    if (!frontend::valid_flat_namespace_name(expression->name))
        fail("provider namespace calls must be unqualified flat member names");

    const auto& selected = resolve_export(
        graph, namespace_key, expression->name, expression->arguments.size());
    const CallableId callee{
        selected.source_unit_id, selected.name, selected.arity};
    edges[caller].insert(callee);
    const auto prefix = prefixes.find(selected.source_unit_id);
    if (prefix == prefixes.end())
        fail("provider call target is not in the reachable namespace group");
    expression->name = prefix->second + selected.name;
}

void reject_provider_call_cycles(
        const std::map<CallableId, std::set<CallableId>>& edges) {
    std::map<CallableId, unsigned char> state;
    std::function<void(const CallableId&)> visit = [&](const CallableId& node) {
        const auto current = state[node];
        if (current == 2) return;
        if (current == 1)
            fail("cyclic namespace provider call graph");
        state[node] = 1;
        const auto found = edges.find(node);
        if (found != edges.end()) {
            for (const auto& child : found->second)
                visit(child);
        }
        state[node] = 2;
    };
    for (const auto& [node, ignored] : edges) {
        (void)ignored;
        visit(node);
    }
}

} // namespace

Program load_pinned_namespace_program(const PinnedNamespaceProgramInput& input) {
    if (!valid_host_identity(input.entry_source_unit_id))
        fail("entry source id is missing, oversized or contains control bytes");
    if (input.sources.empty() || input.sources.size() > kMaxPinnedNamespaceSources)
        fail("source inventory must contain 1..128 units");

    std::size_t aggregate_bytes = 0;
    std::vector<std::unique_ptr<PinnedUnit>> units;
    units.reserve(input.sources.size());
    std::map<std::string, PinnedUnit*> by_source;

    for (const auto& source : input.sources) {
        if (!valid_host_identity(source.source_unit_id)
                || !valid_host_identity(source.package_instance_id))
            fail("every source requires bounded control-free source/package identity");
        if (source.source.size() > kMaxPinnedNamespaceBytes - aggregate_bytes)
            fail("source inventory exceeds 8 MiB aggregate input limit");
        aggregate_bytes += source.source.size();
        if (source.source.find("// ores:") != std::string::npos)
            fail("ores environment headers require separate target verification");

        auto unit = std::make_unique<PinnedUnit>();
        unit->source_unit_id = source.source_unit_id;
        unit->package_instance_id = source.package_instance_id;
        unit->program = parse_unresolved_program(source.source, false, false, true);
        if (!by_source.emplace(unit->source_unit_id, unit.get()).second)
            fail("duplicate source unit identity");
        if (!unit->program.namespace_name.empty() && !unit->program.imports.empty())
            fail("namespace provider imports require a later dependency-aware ABI");
        units.push_back(std::move(unit));
    }

    const auto entry_it = by_source.find(input.entry_source_unit_id);
    if (entry_it == by_source.end())
        fail("entry source is absent from pinned inventory");
    PinnedUnit& entry = *entry_it->second;
    require_entry_scalar_surface(entry.program);
    require_entry_namespace_bindings_unshadowed(entry.program);

    // Closed inventory: every non-entry unit must carry package-owned namespace
    // identity. Merely sharing a directory/package instance never makes a file
    // reachable.
    for (const auto& unit : units) {
        if (unit.get() != &entry && unit->program.namespace_name.empty())
            fail("non-entry source must declare a package-owned namespace");
    }

    std::vector<frontend::NamespaceOwnedSource> owned_sources;
    owned_sources.reserve(units.size());
    std::vector<frontend::NamespaceImportRequest> import_requests;
    for (const auto& unit : units) {
        owned_sources.push_back({
            unit->source_unit_id, unit->package_instance_id, &unit->program});
        if (unit.get() != &entry && !unit->program.imports.empty())
            fail("namespace v1 admits imports only from the entry source");
        for (const auto& imported : unit->program.imports) {
            if (!imported.selected_namespace || imported.selected_class)
                fail("pinned namespace loader admits only import ns dependencies");
            if (imported.local_name != imported.selected_module)
                fail("namespace v1 does not admit rebinding aliases");
            if (imported.path.rfind("pkg:", 0) != 0 || imported.path.size() <= 4)
                fail("namespace import requires exact pkg:<alias> selector");
            import_requests.push_back({
                unit->source_unit_id,
                imported.path.substr(4),
                imported.selected_module
            });
        }
    }
    if (import_requests.empty())
        fail("entry source must import at least one pinned namespace");

    // #1162's host model remains call-free by default. This executable path
    // opts in to structurally scalar calls, then resolves every admitted call
    // below to an exact export and proves provider-call acyclicity.
    const auto graph = frontend::link_package_namespaces(
        owned_sources, input.pinned_aliases, import_requests,
        frontend::PackageNamespaceLinkOptions{true});

    std::set<std::string> reachable_sources;
    std::set<frontend::PackageNamespaceKey> reachable_namespaces;
    for (const auto& imported : entry.program.imports) {
        const auto binding = graph.imports.find({
            entry.source_unit_id, imported.selected_module});
        if (binding == graph.imports.end())
            fail("entry namespace import was not resolved by the verified graph");
        reachable_namespaces.insert(binding->second);
        const auto group = graph.namespaces.find(binding->second);
        if (group == graph.namespaces.end())
            fail("verified namespace group missing during lowering");
        reachable_sources.insert(
            group->second.source_units.begin(), group->second.source_units.end());
    }

    // Stable source-id ordering makes generated internal names independent of
    // caller inventory ordering.
    std::map<std::string, std::string> prefixes;
    std::size_t ordinal = 0;
    for (const auto& source_id : reachable_sources)
        prefixes.emplace(
            source_id, "__ores_namespace_" + std::to_string(ordinal++) + ".");

    rewrite_entry_functions(
        entry.source_unit_id, entry.program, graph, prefixes);

    std::map<std::string, std::set<std::pair<std::string, std::size_t>>>
        authorized_exports;
    for (const auto& namespace_key : reachable_namespaces) {
        const auto& group = graph.namespaces.at(namespace_key);
        for (const auto& exported : group.exports)
            authorized_exports[exported.source_unit_id].insert({
                exported.name, exported.arity});
    }

    // Rewrite provider-to-provider calls only within the exact same
    // (PackageInstanceId, NamespaceName). Cross-namespace and cross-package
    // calls require explicit import/dependency syntax in a later ABI.
    std::map<CallableId, std::set<CallableId>> provider_edges;
    for (const auto& source_id : reachable_sources) {
        PinnedUnit& provider = *by_source.at(source_id);
        const frontend::PackageNamespaceKey namespace_key{
            provider.package_instance_id, provider.program.namespace_name};
        if (!reachable_namespaces.count(namespace_key))
            fail("reachable source does not belong to an imported namespace");
        for (auto& fn : provider.program.functions) {
            const auto signature = std::make_pair(
                fn.name, fn.parameters.size());
            if (authorized_exports[source_id].count(signature) == 0) continue;
            const CallableId caller{source_id, fn.name, fn.parameters.size()};
            provider_edges.try_emplace(caller);
            for (auto& local : fn.locals)
                rewrite_provider_expression(
                    caller, namespace_key, local.initializer.get(),
                    graph, prefixes, provider_edges);
            rewrite_provider_expression(
                caller, namespace_key, fn.result.get(),
                graph, prefixes, provider_edges);
        }
    }
    reject_provider_call_cycles(provider_edges);

    Program linked = std::move(entry.program);
    linked.imports.clear();
    for (const auto& source_id : reachable_sources) {
        PinnedUnit& provider = *by_source.at(source_id);
        std::size_t moved = 0;
        for (auto& fn : provider.program.functions) {
            const auto signature = std::make_pair(
                fn.name, fn.parameters.size());
            if (authorized_exports[source_id].count(signature) == 0) continue;
            // Source-level pub grants namespace visibility, not host ABI
            // visibility. Once authorized and rewritten, provider symbols are
            // linker-owned implementation details.
            fn.native_internal = true;
            fn.name = prefixes.at(source_id) + fn.name;
            linked.functions.push_back(std::move(fn));
            ++moved;
        }
        if (moved != authorized_exports[source_id].size())
            fail("authorized namespace export missing from provider AST");
    }

    // Existing whole-program validation is the final authority for scalar
    // argument/return types, ownership and codegen admission after exact graph
    // resolution. A bad cross-file call therefore cannot bypass normal checks.
    resolve_and_validate_program(linked);
    return linked;
}

} // namespace oreslang
