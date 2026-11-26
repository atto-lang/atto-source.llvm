#pragma once
#include "oreslang/frontend/Parser.hpp"
#include "oreslang/frontend/SourceInput.hpp"
#include "oreslang/frontend/VerifiedNamespaceSourceSnapshotV2.hpp"
#include <filesystem>
#include <string_view>
#include <vector>
#include <cstdint>
#include <string>

namespace oreslang {
// Compile-time only. No source-loader or resolver is linked into guest artifacts.
// Relative module selections are statically resolved before ordinary whole-program
// name/arity/effect/ownership validation. Unsupported source never yields IR.
Program load_native_program(const std::string& source_path, std::string source,
                            SourceIdentity identity, bool experimental_owned_int,
                            bool experimental_rt_scalar_loop, bool host_target,
                            std::uint32_t admitted_provider_bits = 0u);

// Explicit host-side preparatory admission; NOT the guest compile path.
// The inventory and both digest pins are supplied by an independently
// authorized host package/lock authority, never guest source/CLI arguments.
// No v2 namespace execution is enabled by this API.
frontend::VerifiedNamespaceSourceSnapshotV2 admit_native_namespace_sources_v2(
    const std::filesystem::path& trusted_physical_root,
    const std::vector<std::string>& approved_members,
    std::string_view approved_manifest_sha256,
    std::string_view approved_sources_sha256,
    const frontend::NamespaceSha256& trusted_sha256);
}
