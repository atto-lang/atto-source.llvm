#include "oreslang/runtime/TenantVmSignedFsLoader.hpp"
#include <cstring>

namespace oreslang::runtime::vm {
namespace {
struct VerifyContext {
    const SignedFsDescriptor *signed_ceiling = nullptr;
    atto_fs_verified_ceiling verified{};
};
bool verify_exact_sealed_image(const std::uint8_t *image, std::size_t bytes,
                               void *opaque) noexcept {
    if (!opaque || !image || !bytes) return false;
    auto *context = static_cast<VerifyContext *>(opaque);
    const auto& input = *context->signed_ceiling;
    /* Input buffer sizes are checked before calling a C ABI whose signature
     * requires exact fixed-size signature and key buffers. */
    if (!input.wire || !input.wire_size || !input.signature_ed25519 ||
        input.signature_size != ATTO_FS_ED25519_SIGNATURE_BYTES ||
        !input.trusted_public_key ||
        input.public_key_size != ATTO_FS_ED25519_PUBLIC_KEY_BYTES) return false;
    return atto_fs_verify_signed_ceiling_v1(image, bytes,
                input.wire, input.wire_size, input.signature_ed25519,
                input.trusted_public_key, &context->verified) == 0;
}
} // namespace

LoadedSignedFsGuest load_guest_library_with_signed_fs_ceiling(
    const std::string& path, const std::string& expected,
    const SignedFsDescriptor& signature) {
    LoadedSignedFsGuest result;
    if (!signature.wire || !signature.wire_size ||
        !signature.signature_ed25519 ||
        signature.signature_size != ATTO_FS_ED25519_SIGNATURE_BYTES ||
        !signature.trusted_public_key ||
        signature.public_key_size != ATTO_FS_ED25519_PUBLIC_KEY_BYTES) {
        result.guest.error = "signed FS ceiling input is missing or malformed";
        return result;
    }
    VerifyContext context{&signature, {}};
    result.guest = load_guest_library_checked(path, expected,
                                              &verify_exact_sealed_image, &context);
    if (!result.guest.module) {
        /* Do not return apparent authority after loader/ABI failure. */
        std::memset(&context.verified, 0, sizeof(context.verified));
        return result;
    }
    result.verified_artifact_ceiling = context.verified;
    result.signature_verified = true;
    std::memset(&context.verified, 0, sizeof(context.verified));
    return result;
}
namespace {
const char* nonempty_scope(const char* value) noexcept {
    return value && value[0] ? value : nullptr;
}
}

int admit_signed_loaded_guest_fs_broker(
    atto_fs_broker *out, const LoadedSignedFsGuest& loaded,
    const std::string& trusted_root,
    const TrustedFsExecutionCeilings& ceilings) {
    /* No re-reading of file paths or separate artifact policy injection.
     * The signing verifier already checked the precise bytes that dlopen
     * mapped via the loader's sealed private image. All other ceilings
     * remain independently host-provided and are NOT authenticated here. */
    if (!out || !loaded.signature_verified || !loaded.guest.module ||
        !loaded.guest.owner ||
        loaded.guest.module->abi_version != ORES_VM_GUEST_ABI_VERSION ||
        trusted_root.empty() || ceilings.tenant_id == 0 ||
        ceilings.generation == 0) return -EACCES;
    const auto& artifact = loaded.verified_artifact_ceiling;
    atto_fs_layered_policy layers{};
    layers.tenant_id = ceilings.tenant_id;
    layers.generation = ceilings.generation;
    layers.host = ceilings.host;
    layers.artifact = {
        nonempty_scope(artifact.allow_read),
        nonempty_scope(artifact.allow_write),
        nonempty_scope(artifact.deny_read),
        nonempty_scope(artifact.deny_write)
    };
    layers.runtime = ceilings.runtime;
    layers.tenant = ceilings.tenant;
    layers.actor = ceilings.actor;
    layers.delegation = ceilings.delegation;
    return atto_fs_broker_init_layers(out, trusted_root.c_str(), &layers);
}


int admit_loaded_guest_fs_broker(atto_fs_broker *broker,
                                  const LoadedSignedFsGuest& verified,
                                  const TrustedLoadedFsLimits& limits) {
    /* No re-reading a path and no independently caller-supplied artifact
     * policy: use the exact image verified by the sealed guest loader.
     * This is still a host-only function; all callers and limits are trusted. */
    if (!broker || broker->initialized || !verified.signature_verified ||
        !verified.guest.module || !verified.guest.owner ||
        !limits.trusted_root || !limits.tenant_id || !limits.generation)
        return -EACCES;
    const auto& ceiling = verified.verified_artifact_ceiling;
    const auto scope = [](const char *s) -> const char * {
        return s && s[0] ? s : nullptr;
    };
    const atto_fs_layered_policy policy = {
        limits.tenant_id, limits.generation, limits.host,
        {scope(ceiling.allow_read), scope(ceiling.allow_write),
         scope(ceiling.deny_read), scope(ceiling.deny_write)},
        limits.runtime, limits.tenant, limits.actor, limits.delegation
    };
    return atto_fs_broker_init_layers(broker, limits.trusted_root, &policy);
}

} // namespace oreslang::runtime::vm
