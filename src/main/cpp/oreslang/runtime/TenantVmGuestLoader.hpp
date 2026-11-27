#pragma once
// Loads a lowered guest library for one or more tenant VMs. The digest is the admission: only the
// output of the lowering pipeline should ever be named here, and the bytes that get mapped are
// the bytes that were hashed. Tenants never supply these files.
#include "oreslang/runtime/TenantVmGuestAbi.h"

#include <cstdint>
#include <cstddef>
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

/* INTERNAL TRUSTED-HOST-ONLY image admission hook. Executed against the
 * EXACT bytes that the loader copies into its sealed/private executable
 * image, after SHA-256 admission but before any dlopen.
 *
 * The callback is host code, never guest code. A failure must abort before
 * loading. The legacy ABI-v4 loader remains available for unprivileged
 * modules; privileged std.fs modules MUST go through verified signed
 * admission once ABI v5 is supported.
 */
using GuestPreloadVerifier = bool (*)(const std::uint8_t* bytes,
                                      std::size_t count, void* context) noexcept;
LoadedGuest load_guest_library_checked(const std::string& path,
                                        const std::string& sha256_hex,
                                        GuestPreloadVerifier verifier,
                                        void* context);


std::string sha256_hex(const void* data, std::size_t size);

} // namespace oreslang::runtime::vm
