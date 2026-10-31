#pragma once

#include "oreslang/frontend/PinnedNamespaceLoader.hpp"

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace oreslang {

// Serialization-neutral lock authority. A future standardized lockfile parser
// may populate this structure, but executable namespace lowering consumes only
// exact, already-verified package-instance pins.
struct VerifiedPackageLockSnapshot {
    std::string root_package_instance_id;
    std::map<std::string, std::string> dependency_aliases;
};

// Strict schema-v2 package manifest subset needed by namespace source ownership.
// Unknown package/source constructs fail closed rather than being ignored.
struct VerifiedPackageManifestV2 {
    std::filesystem::path manifest_path;
    std::filesystem::path package_root;
    std::string project_name;
    std::vector<std::filesystem::path> source_files;
};

// Reads and validates the modern package manifest at an exact canonical path.
// V2 accepts:
//   schema_version = "2"
//   [project] name = "..."; root = "." or canonical relative path
//   [packages.<project-name>] files = ["...ores", ...]
// Other fields needed by future package resolution remain unsupported.
VerifiedPackageManifestV2 load_verified_package_manifest_v2(
    const std::filesystem::path& manifest_path,
    const std::function<std::string(const std::filesystem::path&)>& read_manifest);

// Builds the exact pinned namespace compiler input from one root application
// manifest, one or more verified dependency manifests, and an already-verified
// package lock snapshot. No directory scan, ORES_PATHS fallback, or namespace
// spelling fallback is permitted.
PinnedNamespaceProgramInput build_pinned_namespace_program_from_manifests(
    const VerifiedPackageManifestV2& root_manifest,
    const std::vector<VerifiedPackageManifestV2>& dependency_manifests,
    const VerifiedPackageLockSnapshot& lock,
    const std::filesystem::path& entry_source,
    const std::function<std::string(const std::filesystem::path&)>& read_source);

} // namespace oreslang
