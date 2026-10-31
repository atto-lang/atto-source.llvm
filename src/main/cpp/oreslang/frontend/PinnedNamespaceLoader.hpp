#pragma once

#include "oreslang/frontend/PackageNamespaceGraph.hpp"

#include <string>
#include <vector>

namespace oreslang {

// Host-authenticated source bytes and package ownership. This API does not
// discover packages, trust ambient directories, or authenticate a lockfile;
// callers must supply an already verified closed package/source inventory.
struct PinnedNamespaceSourceInput {
    std::string source_unit_id;
    std::string package_instance_id;
    std::string source;
};

struct PinnedNamespaceProgramInput {
    std::string entry_source_unit_id;
    std::vector<PinnedNamespaceSourceInput> sources;
    frontend::PackageAliasPins pinned_aliases;
};

// Compile-time-only scalar namespace lowering. The ordinary filesystem/CLI
// loader remains fail-closed for import ns; this path requires explicit pinned
// package/source ownership and emits one ordinary validated Program for the
// existing LLVM backend.
Program load_pinned_namespace_program(const PinnedNamespaceProgramInput& input);

} // namespace oreslang
