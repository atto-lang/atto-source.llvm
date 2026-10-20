#pragma once

#include "oreslang/frontend/Parser.hpp"

#include <string>
#include <vector>

namespace oreslang {

enum class TreeShakeMode { Executable, Library };

/**
 * Native subset implementation of the backend-neutral
 * oreslang.reachability/1 contract.
 *
 * v1 removes unreachable top-level functions. Classes and actors remain
 * conservative roots until member/open-world semantics are shared with the
 * common compiler contract.
 */
struct NativeReachabilityManifest {
    std::vector<std::string> entry_points;
    bool preserve_public_api = false;
    std::vector<std::string> retained_symbols;
    std::vector<std::string> removed_symbols;
    std::vector<std::string> external_bindings;
    std::vector<std::string> language_features;
    std::vector<std::string> runtime_operations;

    std::string to_canonical_json() const;
};

/**
 * Prune unreachable top-level functions in-place after complete semantic and
 * target admission. Missing or ambiguous roots/calls fail closed.
 */
NativeReachabilityManifest tree_shake_program(Program& program, TreeShakeMode mode);

} // namespace oreslang
