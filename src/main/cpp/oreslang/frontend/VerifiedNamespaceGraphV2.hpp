#pragma once

#include "oreslang/frontend/PackageNamespaceGraph.hpp"
#include "oreslang/frontend/VerifiedNamespaceDependencyLockV2.hpp"
#include "oreslang/frontend/VerifiedNamespaceSourceSnapshotV2.hpp"
#include <functional>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// This bridge consumes immutable v2 source bytes already admitted against
// independently trusted pins, not a filesystem path or an arbitrary source
// owner supplied by guest code. Host-only; no executable LLVM lowering yet.
namespace oreslang::frontend {

using NamespaceParseCapturedSourceV2 = std::function<Program(std::string_view)>;

struct VerifiedNamespacePackageInputV2 {
    const VerifiedNamespaceSourceSnapshotV2* snapshot = nullptr;
};

// External aliases are admitted only through a sealed lockfile whose hash
// was independently authorized. The owner set MUST still match the
// physically verified snapshots supplied in this specific invocation.
inline PackageNamespaceGraph link_verified_namespace_snapshots_v2(
        const std::vector<VerifiedNamespacePackageInputV2>& bundles,
        const NamespaceParseCapturedSourceV2& parse_captured_source,
        const VerifiedNamespaceDependencyLockV2* verified_lock = nullptr) {
    if (!parse_captured_source || bundles.empty() || bundles.size() > 33)
        throw std::invalid_argument("v2 namespace requires bounded verified snapshots and host parser");

    std::size_t source_count = 0;
    std::vector<std::pair<std::string, std::string>> owned_ids;
    std::vector<const std::string*> captured_bytes;
    std::map<std::string, std::string> observed_owner;
    std::set<std::string> physical_owners;
    for (const auto& bundle : bundles) {
        if (!bundle.snapshot)
            throw std::invalid_argument("v2 namespace package snapshot missing");
        const auto& verified = bundle.snapshot->inventory();
        const auto& files = bundle.snapshot->source_bytes();
        if (verified.package_files().empty())
            throw std::invalid_argument("v2 namespace package has no source inventory");
        if (files.empty() || files.size() > 4096 ||
                source_count > 4096 - files.size())
            throw std::invalid_argument("v2 namespace source inventory exceeds bound");
        source_count += files.size();

        for (const auto& entry : verified.package_files()) {
            if (!namespace_v2_name(entry.first))
                throw std::invalid_argument("v2 namespace invalid verified package name");
            // Identity contains BOTH independently pinned digests and the
            // manifest package key. Never derive ownership from a namespace
            // header, source path, working directory or global spelling.
            const std::string owner = "v2:" + verified.manifest_sha256() + ":"
                + verified.sources_sha256() + ":" + entry.first;
            physical_owners.insert(owner);
            for (const auto& relative : entry.second) {
                const auto found = files.find(relative);
                if (found == files.end())
                    throw std::invalid_argument("v2 namespace missing captured provider bytes");
                const std::string id = owner + "/" + relative;
                if (!observed_owner.emplace(id, owner).second)
                    throw std::invalid_argument("v2 namespace duplicate physical source identity");
                owned_ids.emplace_back(id, owner);
                captured_bytes.push_back(&found->second);
            }
        }
    }
    if (owned_ids.size() != source_count)
        throw std::invalid_argument("v2 namespace source inventory does not equal captured bytes");
    if (verified_lock) {
        for (const auto& [importer, aliases] : verified_lock->alias_pins()) {
            if (!physical_owners.count(importer))
                throw std::invalid_argument("v2 namespace locked importer lacks physical verified snapshot");
            for (const auto& [alias, provider] : aliases) {
                if (!namespace_lock_name_v2(alias) || !physical_owners.count(provider))
                    throw std::invalid_argument("v2 namespace locked target lacks physical verified snapshot");
            }
        }
    }

    // Reserve storage before linking: PackageNamespaceGraph takes temporary
    // pointers into these checked native ASTs and must not observe reallocations.
    std::vector<Program> parsed;
    parsed.reserve(source_count);
    std::vector<NamespaceImportRequest> imports;
    for (std::size_t i = 0; i < owned_ids.size(); ++i) {
        Program program = parse_captured_source(*captured_bytes[i]);
        if (!program.imports.empty())
            throw std::invalid_argument("v2 namespace legacy module imports lack verified ownership");
        for (const auto& imported : program.namespace_imports) {
            if (imported.package_selector != "+/") {
                if (!verified_lock)
                    throw std::invalid_argument(
                        "v2 namespace external dependency imports require verified lockfile pins");
                const auto owner = verified_lock->alias_pins().find(owned_ids[i].second);
                if (owner == verified_lock->alias_pins().end() ||
                        owner->second.count(imported.package_selector) == 0)
                    throw std::invalid_argument(
                        "v2 namespace external dependency imports require verified lockfile pins");
            }
            imports.push_back({owned_ids[i].first, imported.package_selector,
                imported.namespace_name, imported.local_name});
            if (imports.size() > 4096)
                throw std::invalid_argument("v2 namespace import edge budget exceeded");
        }
        // Every embedded import edge is now bound to a verified physical
        // source. Graph admission otherwise correctly rejects an unchecked
        // Program.namespace_imports payload (including on root callers).
        program.namespace_imports.clear();
        parsed.push_back(std::move(program));
    }
    std::vector<NamespaceOwnedSource> owned;
    owned.reserve(source_count);
    for (std::size_t i = 0; i < owned_ids.size(); ++i)
        owned.push_back({owned_ids[i].first, owned_ids[i].second, &parsed[i]});

    // Never infer an alias from source spelling or a global provider search.
    // The lock is independently pinned and has been checked against all
    // physical owner instances before becoming a graph input.
    return link_package_namespaces(
        owned, verified_lock ? verified_lock->alias_pins() : PackageAliasPins{}, imports);
}

// Production parser composition is deliberately host-only and returns only
// the checked owner graph. The AST and captures never enter executable LLVM
// lowering through this path. Once exact-head tests qualify the parser
// bridge, codegen must still authorize each scalar call independently.
inline PackageNamespaceGraph link_verified_native_namespace_snapshots_v2(
        const std::vector<VerifiedNamespacePackageInputV2>& bundles,
        const VerifiedNamespaceDependencyLockV2* verified_lock = nullptr) {
    return link_verified_namespace_snapshots_v2(
        bundles, [](std::string_view captured_source) {
            return parse_verified_namespace_source_unresolved(captured_source);
        }, verified_lock);
}

} // namespace oreslang::frontend
