#pragma once
/*
 * Trusted-host-only signed filesystem ceiling admission for an EXACT sealed
 * guest image, before any dlopen. This is not a v5 guest admission path yet:
 * current TenantVm loader still admits only ABI v4.
 */
#include "oreslang/runtime/TenantVmGuestLoader.hpp"
#include "atto/host_fs_signed_ceiling.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace oreslang::runtime::vm {
struct SignedFsDescriptor {
    const std::uint8_t* wire = nullptr;
    std::size_t wire_size = 0;
    const std::uint8_t* signature_ed25519 = nullptr;
    std::size_t signature_size = 0;
    const std::uint8_t* trusted_public_key = nullptr;
    std::size_t public_key_size = 0;
};
struct LoadedSignedFsGuest {
    LoadedGuest guest;
    /* Plain, mutable trusted-host state. It is NOT a guest capability and
     * must remain private. Only a trusted runtime may use these fields to
     * construct the artifact layer of the six-ceiling broker. */
    atto_fs_verified_ceiling verified_artifact_ceiling{};
    bool signature_verified = false;
};
/* Verifies against the exact byte vector which open_verified() copies into a
 * sealed private image. No second pathname check; fail before dlopen.
 * Does not prove the guest is safe for in-process hostile tenant execution. */
LoadedSignedFsGuest load_guest_library_with_signed_fs_ceiling(
    const std::string& path, const std::string& expected_sha256_hex,
    const SignedFsDescriptor& signature);
}
