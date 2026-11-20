#pragma once
/*
 * Trusted-host-only signed filesystem ceiling admission for an EXACT sealed
 * guest image, before any dlopen. This is not a v5 guest admission path yet:
 * current TenantVm loader still admits only ABI v4.
 */
#include "oreslang/runtime/TenantVmGuestLoader.hpp"
#include "atto/host_fs_signed_ceiling.h"
#include "atto/host_fs_broker.h"

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
/* Non-artifact ceilings MUST originate in trusted host/scheduler state;
 * this struct is never mapped into guest memory. */
struct TrustedFsExecutionCeilings {
    std::uint64_t tenant_id = 0;
    std::uint64_t generation = 0;
    atto_fs_policy_layer host{};
    atto_fs_policy_layer runtime{};
    atto_fs_policy_layer tenant{};
    atto_fs_policy_layer actor{};
    atto_fs_policy_layer delegation{};
};

/* Creates the six-layer broker ONLY when a signed library was successfully
 * loaded from verified bytes. The artifact layer is sourced from that
 * verifier's private result; no caller-supplied artifact policy is accepted.
 * A signed ABI-v4 guest remains UNPRIVILEGED; this is trusted host setup,
 * not a guest-visible capability or runtime ABI-v5 admission.
 *
 * out must be fresh (zero-initialized), single-owner, and closed only after
 * every worker has quiesced. loaded.guest.owner must outlive the broker.
 */
int admit_signed_loaded_guest_fs_broker(
    atto_fs_broker *out, const LoadedSignedFsGuest& loaded,
    const std::string& trusted_root,
    const TrustedFsExecutionCeilings& ceilings);

LoadedSignedFsGuest load_guest_library_with_signed_fs_ceiling(
    const std::string& path, const std::string& expected_sha256_hex,
    const SignedFsDescriptor& signature);

/* Trusted host admission from a VERIFIED, loaded and still-owned sealed
 * executable image. The caller provides five independently validated and
 * scoped ceilings; the artifact ceiling comes ONLY from the previously
 * verified loader result, not from an unsigned launch field.
 * This function never takes guest input, does not mint actor capabilities,
 * and is NOT the still-unimplemented guest ABI v5 dispatch. */
struct TrustedLoadedFsLimits {
    const char *trusted_root = nullptr;
    std::uint64_t tenant_id = 0;
    std::uint64_t generation = 0;
    atto_fs_policy_layer host{};
    atto_fs_policy_layer runtime{};
    atto_fs_policy_layer tenant{};
    atto_fs_policy_layer actor{};
    atto_fs_policy_layer delegation{};
};
int admit_loaded_guest_fs_broker(atto_fs_broker *broker,
                                  const LoadedSignedFsGuest& verified,
                                  const TrustedLoadedFsLimits& limits);
}
