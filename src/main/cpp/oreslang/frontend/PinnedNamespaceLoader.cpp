#include "oreslang/frontend/PinnedNamespaceLoader.hpp"

#include <algorithm>
#include <cstddef>
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

struct PinnedUnit {
    std::string source_unit_id;
    std::string package_instance_id;
    Program program;
};

[[noreturn]] void fail(const std::string& reason) {
    throw std::runtime_error("pinned namespace lowering: " + reason);
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

void rewrite_namespace_expression(
        const std::string& source_unit_id,
        Expression* expression,
        const frontend::PackageNamespaceGraph& graph,
        const std::map<std::string, std::string>& prefixes) {
    if (!expression) return;
    rewrite_namespace_expression(source_unit_id, expression->left.get(), graph, prefixes);
    rewrite_namespace_expression(source_unit_id, expression->right.get(), graph, prefixes);
    rewrite_namespace_expression(source_unit_id, expression->condition.get(), graph, prefixes);
    rewrite_namespace_expression(source_unit_id, expression->condition_lhs.get(), graph, prefixes);
    rewrite_namespace_expression(source_unit_id, expression->condition_rhs.get(), graph, prefixes);
    rewrite_namespace_expression(source_unit_id, expression->when_true.get(), graph, prefixes);
    rewrite_namespace_expression(source_unit_id, expression->when_false.get(), graph, prefixes);
    for (auto& argument : expression->arguments)
        rewrite_namespace_expression(source_unit_id, argument.get(), graph, prefixes);

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
    const auto group = graph.namespaces.find(imported->second);
    if (group == graph.namespaces.end())
        fail("resolved namespace target disappeared from verified graph");

    const frontend::PackageNamespaceExport* selected = nullptr;
    for (const auto& exported : group->second.exports) {
        if (exported.name != member || exported.arity != expression->arguments.size())
            continue;
        if (selected)
            fail("ambiguous namespace callable after graph validation");
        selected = &exported;
    }
    if (!selected)
        fail("unknown namespace member or wrong arity '" + expression->name + "'");
    const auto prefix = prefixes.find(selected->source_unit_id);
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
            rewrite_namespace_expression(source_unit_id, local.initializer.get(), graph, prefixes);
        rewrite_namespace_expression(source_unit_id, fn.result.get(), graph, prefixes);
    }
}

} // namespace

Program load_pinned_namespace_program(const PinnedNamespaceProgramInput& input) {
    if (input.entry_source_unit_id.empty())
        fail("entry source id is required");
    if (input.sources.empty() || input.sources.size() > kMaxPinnedNamespaceSources)
        fail("source inventory must contain 1..128 units");

    std::size_t aggregate_bytes = 0;
    std::vector<std::unique_ptr<PinnedUnit>> units;
    units.reserve(input.sources.size());
    std::map<std::string, PinnedUnit*> by_source;

    for (const auto& source : input.sources) {
        if (source.source_unit_id.empty() || source.package_instance_id.empty())
            fail("every source requires explicit source and package instance identity");
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

        // Namespace providers are independently complete scalar compilation
        // units. Resolve them before graph admission so every scalar type and
        // lexical binding checked by PackageNamespaceGraph is compiler-derived.
        if (!unit->program.namespace_name.empty()) {
            if (!unit->program.imports.empty())
                fail("namespace provider imports require a later dependency-aware ABI");
            resolve_and_validate_program(unit->program);
        }
        units.push_back(std::move(unit));
    }

    const auto entry_it = by_source.find(input.entry_source_unit_id);
    if (entry_it == by_source.end())
        fail("entry source is absent from pinned inventory");
    PinnedUnit& entry = *entry_it->second;
    require_entry_scalar_surface(entry.program);

    // Initial executable slice is a closed app entry plus namespace provider
    // files. Extra non-namespace roots would otherwise become ambient inputs.
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

    const auto graph = frontend::link_package_namespaces(
        owned_sources, input.pinned_aliases, import_requests);

    // Only namespace groups imported by the entry are emitted. The complete
    // package inventory is still graph-validated, including same-named
    // namespaces in other package instances, so no ambient/global fallback is
    // possible while unused namespace code remains out of the linked guest.
    std::set<std::string> reachable_sources;
    for (const auto& imported : entry.program.imports) {
        const auto binding = graph.imports.find({
            entry.source_unit_id, imported.selected_module});
        if (binding == graph.imports.end())
            fail("entry namespace import was not resolved by the verified graph");
        const auto group = graph.namespaces.find(binding->second);
        if (group == graph.namespaces.end())
            fail("verified namespace group missing during lowering");
        reachable_sources.insert(
            group->second.source_units.begin(), group->second.source_units.end());
    }

    std::map<std::string, std::string> prefixes;
    std::size_t ordinal = 0;
    for (const auto& source_id : reachable_sources)
        prefixes.emplace(source_id, "__ores_namespace_" + std::to_string(ordinal++) + ".");

    rewrite_entry_functions(entry.source_unit_id, entry.program, graph, prefixes);

    // Record exactly the public signatures authorized by the graph, indexed by
    // verified provider source. Private helpers are intentionally omitted in
    // this first call-free provider slice.
    std::map<std::string, std::set<std::pair<std::string, std::size_t>>> authorized_exports;
    for (const auto& imported : entry.program.imports) {
        const auto target = graph.imports.at({
            entry.source_unit_id, imported.selected_module});
        const auto& group = graph.namespaces.at(target);
        for (const auto& exported : group.exports)
            authorized_exports[exported.source_unit_id].insert({
                exported.name, exported.arity});
    }

    Program linked = std::move(entry.program);
    linked.imports.clear();
    for (const auto& source_id : reachable_sources) {
        auto unit_it = by_source.find(source_id);
        if (unit_it == by_source.end())
            fail("reachable namespace source is absent from pinned inventory");
        PinnedUnit& provider = *unit_it->second;
        std::size_t moved = 0;
        for (auto& fn : provider.program.functions) {
            const auto signature = std::make_pair(fn.name, fn.parameters.size());
            if (authorized_exports[source_id].count(signature) == 0) continue;
            fn.name = prefixes.at(source_id) + fn.name;
            linked.functions.push_back(std::move(fn));
            ++moved;
        }
        if (moved != authorized_exports[source_id].size())
            fail("authorized namespace export missing from provider AST");
    }

    resolve_and_validate_program(linked);
    return linked;
}

} // namespace oreslang
