#pragma once
// Loads a lowered guest library for one or more tenant VMs.
// The caller-supplied digest checks byte integrity only: it does NOT authenticate
// trusted-compiler provenance, source identity or permission to execute native code.
// Dynamic libraries execute in the host OS process with its native authority.
// Do not accept tenant-supplied libraries OR caller-supplied expected hashes as
// sufficient authorization for hostile/untrusted execution. A separately trusted,
// immutable compiler/attestation policy must authorize the exact artifact first.
// Tenant VM ABI addressing restrictions are not enforced by this loader.
#include "oreslang/runtime/TenantVmGuestAbi.h"

#include <memory>
#include <string>

namespace oreslang::runtime::vm {

struct LoadedGuest {
    const OresVmGuestModule* module = nullptr;
    // Put this in VmSpec::module_owner. The library stays mapped until the last VM using it retires.
    std::shared_ptr<const void> owner;
    std::string error; // empty on success
};

// sha256_hex is the lowercase digest recorded when the library was lowered.
LoadedGuest load_guest_library(const std::string& path, const std::string& sha256_hex);

std::string sha256_hex(const void* data, std::size_t size);

} // namespace oreslang::runtime::vm
