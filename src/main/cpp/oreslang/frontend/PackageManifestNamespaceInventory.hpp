#pragma once

#include "oreslang/frontend/PinnedNamespaceLoader.hpp"

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace oreslang {

// Serialization-neutral immutable lock entry. The future standardized lockfile
// parser should populate exactly this authority; executable namespace lowering
// never infers package identity from project names, folders or namespace text.
struct VerifiedLockedPackageInstance {
    std::filesystem::path manifest_path;
    std::string manifest_sha256;
    // Manifest-relative source spelling -> exact SHA-256 content.
    std::map<std::string, std::string> source_sha256;
};

struct VerifiedPackageLockSnapshot {
    std::string root_package_instance_id;
    std::map<std::string, VerifiedLockedPackageInstance> package_instances;
    // Dependency aliases visible to the root package in this first vertical
    // slice. Every target must name package_instances exactly.
    std::map<std::string, std::string> dependency_aliases;
};

struct VerifiedPackageSourceMember {
    std::string manifest_relative_path;
    std::filesystem::path physical_path;
};

// Strict schema-v2 package manifest subset needed by namespace source ownership.
// Unknown package/source constructs fail closed rather than being ignored.
struct VerifiedPackageManifestV2 {
    std::filesystem::path manifest_path;
    std::filesystem::path package_root;
    std::string project_name;
    std::string manifest_sha256;
    std::vector<VerifiedPackageSourceMember> source_files;
};

// Stable lowercase SHA-256 used to compare the already-verified lock snapshot
// with the exact bytes consumed by this compiler slice.
std::string namespace_inventory_sha256(std::string_view bytes);

// Reads and validates the modern package manifest at an exact physical path.
// V2 accepts:
//   schema_version = "2"
//   [project] name = "..."; root = "." or canonical relative path
//   [source] roots = ["..."] (optional; defaults to ["."])
//   [packages.<project-name>] files = ["...ores", ...]
// It never scans directories to discover package members.
VerifiedPackageManifestV2 load_verified_package_manifest_v2(
    const std::filesystem::path& manifest_path,
    const std::function<std::string(const std::filesystem::path&)>& read_manifest);

// Builds the exact pinned namespace compiler input from one root application
// manifest, one or more verified dependency manifests, and an already-verified
// package lock snapshot. No directory scan, ORES_PATHS/ORESLANG_PATH fallback,
// current-working-directory fallback, or namespace spelling fallback is
// permitted. Manifest and source SHA-256 values must match the lock snapshot.
PinnedNamespaceProgramInput build_pinned_namespace_program_from_manifests(
    const VerifiedPackageManifestV2& root_manifest,
    const std::vector<VerifiedPackageManifestV2>& dependency_manifests,
    const VerifiedPackageLockSnapshot& lock,
    const std::filesystem::path& entry_source,
    const std::function<std::string(const std::filesystem::path&)>& read_source);

} // namespace oreslang
