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
} // namespace oreslang::runtime::vm
