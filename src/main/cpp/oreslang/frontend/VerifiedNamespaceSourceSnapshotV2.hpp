#pragma once

#include "oreslang/frontend/NamespacePhysicalSourceV2.hpp"
#include "oreslang/frontend/VerifiedNamespaceManifestV2.hpp"
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oreslang::frontend {
// The only source bytes eligible for future LLVM namespace parsing are these
// captured, pinned bytes. Do not re-open paths after admission. Immutable
// published state prevents the linker modifying source inventories later.
class VerifiedNamespaceSourceSnapshotV2 {
public:
    const VerifiedNamespaceManifestV2& inventory() const { return inventory_; }
    const std::map<std::string, std::string>& source_bytes() const { return source_bytes_; }
private:
    VerifiedNamespaceSourceSnapshotV2(VerifiedNamespaceManifestV2 inventory,
                                       std::map<std::string, std::string> sources)
        : inventory_(std::move(inventory)), source_bytes_(std::move(sources)) {}
    VerifiedNamespaceManifestV2 inventory_;
    std::map<std::string, std::string> source_bytes_;
    friend VerifiedNamespaceSourceSnapshotV2 load_verified_namespace_source_snapshot_v2(
        const std::filesystem::path&, const std::vector<std::string>&,
        std::string_view, std::string_view, const NamespaceSha256&);
};

// The expected member set AND both SHA-256 pins MUST come from a separately
// authorized host package/lock authority. They must never be constructed by
// hashing the same untrusted filesystem just opened here. This entry point
// is host-only, not reachable from guest source or the CLI module resolver.
inline VerifiedNamespaceSourceSnapshotV2 load_verified_namespace_source_snapshot_v2(
        const std::filesystem::path& trusted_physical_root,
        const std::vector<std::string>& approved_members,
        std::string_view approved_manifest_sha256,
        std::string_view approved_sources_sha256,
        const NamespaceSha256& trusted_sha256) {
    auto raw = read_namespace_v2_physical_files(trusted_physical_root, approved_members);
    auto inventory = verify_namespace_manifest_v2(raw.manifest_bytes,
        approved_manifest_sha256, raw.source_bytes, approved_sources_sha256, trusted_sha256);
    return VerifiedNamespaceSourceSnapshotV2(
        std::move(inventory), std::move(raw.source_bytes));
}
} // namespace oreslang::frontend
