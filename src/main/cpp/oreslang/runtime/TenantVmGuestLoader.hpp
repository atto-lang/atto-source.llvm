#pragma once
// Loads a lowered guest library for one or more tenant VMs. The digest is the admission: only the
// output of the lowering pipeline should ever be named here, and the bytes that get mapped are
// the bytes that were hashed. Tenants never supply these files.
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
