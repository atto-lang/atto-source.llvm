#pragma once

#include "oreslang/frontend/Parser.hpp"
#include "oreslang/frontend/NativeOresiV1Admission.hpp"
#include "oreslang/backend/NativeSourceLogManifestV1.hpp"
#include <cstddef>
#include <string>
#include <vector>

namespace oreslang {

// Typed checked source IDs live in the standalone, host-testable
// NativeSourceLogManifestV1.hpp. This codegen performs an independent
// post-frontend exact callable/site inventory check before IR publication.

// Construct LLVM IR for the validated i64-only fragment, verify before emitting.
std::string emit_verified_llvm_ir(const Program& source, bool wasm32_target = false,
                                  bool cortex_m_target = false,
                                  bool bounded_allocator_provider = false,
                                  const std::string& isolate_entry = {},
                                  bool actor_region_v1 = false,
                                  const std::vector<CheckedSourceRoutineV1>& checked_routines = {},
                                  const std::vector<CheckedLogSiteV1>& checked_sites = {},
                                  bool tenant_vm_guest = false);

// Host-owned, restricted interface-only consumer. The host independently
// admits the release and chooses the namespace module and logical slot.
// No filesystem source resolution or arbitrary foreign symbol authority.
struct VerifiedScalarNamespaceBinding {
    const package_interface::Verified& interface;
    package_interface::ScalarLogicalSlotPin slot;
    std::string module;
};
std::string emit_verified_scalar_namespace_consumer(
    std::string_view admitted_consumer_source,
    const std::vector<VerifiedScalarNamespaceBinding>& bindings);

// Independently approved source digest and interface release are both required.
// This produces IR only, not a signed or admitted native object.
std::string emit_verified_scalar_namespace_provider(
    std::string_view admitted_provider_source,
    std::string_view expected_source_sha256,
    const VerifiedScalarNamespaceBinding& binding,
    std::string_view namespace_name,
    std::string_view function_name);

// Opt-in scalar-only continuation IR; external host owns and pins frame storage.
std::string emit_scalar_while_frame_llvm_ir(const ScalarWhileLoop& loop);

} // namespace oreslang
