#pragma once
#include "oreslang/frontend/NativeOresiV1Admission.hpp"
#include "oreslang/frontend/NativeScalarElfAdmission.hpp"
#include <utility>

namespace oreslang::package_interface {

// Trusted host supplies this record independently of candidate object bytes.
// This is an integrity boundary, not registry authentication or an ELF parser.
struct NativeScalarObjectPin {
    ScalarLogicalSlotPin slot;
    std::string namespace_name, module_name, function_name;
    std::string expected_source_sha256, expected_object_sha256;
    std::string physical_symbol;
    std::string target_triple;
    std::string optimization_profile;
};

class VerifiedNativeScalarObject {
    const std::string release_, symbol_, source_digest_, object_digest_;
    const std::string target_, profile_;
    const std::vector<std::uint8_t> bytes_;
    friend VerifiedNativeScalarObject admit_native_scalar_object(
        const Verified&, const NativeScalarObjectPin&, std::string_view,
        std::string_view, std::string_view, std::vector<std::uint8_t>);
    VerifiedNativeScalarObject(std::string release, std::string symbol,
        std::string source_digest, std::string object_digest,
        std::string target, std::string profile, std::vector<std::uint8_t> bytes)
        : release_(std::move(release)), symbol_(std::move(symbol)),
          source_digest_(std::move(source_digest)), object_digest_(std::move(object_digest)),
          target_(std::move(target)), profile_(std::move(profile)), bytes_(std::move(bytes)) {}
public:
    const std::string& release() const noexcept { return release_; }
    const std::string& symbol() const noexcept { return symbol_; }
    const std::string& source_digest() const noexcept { return source_digest_; }
    const std::string& object_digest() const noexcept { return object_digest_; }
    const std::string& target() const noexcept { return target_; }
    const std::string& profile() const noexcept { return profile_; }
    const std::vector<std::uint8_t>& bytes() const noexcept { return bytes_; }
};

inline VerifiedNativeScalarObject admit_native_scalar_object(
        const Verified& proof, const NativeScalarObjectPin& pin,
        std::string_view expected_source, std::string_view expected_target,
        std::string_view expected_profile, std::vector<std::uint8_t> bytes) {
    // Narrow first physical ABI. Never allow a matching guest-supplied digest
    // to stand in for a host lock record or target/profile selection.
    if (expected_target != "x86_64-pc-linux-gnu" ||
        pin.target_triple != expected_target ||
        (expected_profile!="O0" && expected_profile!="O2") ||
        pin.optimization_profile != expected_profile)
        detail::reject("native object target/profile mismatch");
    if (!detail::sha_hex(pin.expected_source_sha256) ||
        pin.expected_source_sha256!=expected_source ||
        !detail::sha_hex(pin.expected_object_sha256))
        detail::reject("native object source/digest pin mismatch");
    const auto symbol=stable_scalar_link_symbol(proof,pin.slot,pin.namespace_name,
                                                pin.module_name,pin.function_name);
    if(symbol!=pin.physical_symbol)
        detail::reject("native object physical symbol pin mismatch");
    if(bytes.empty() || bytes.size()>16u*1024u*1024u ||
        sha256(bytes)!=pin.expected_object_sha256)
        detail::reject("native object bytes digest mismatch");
    elf_scalar_detail::verify(bytes,symbol);
    return VerifiedNativeScalarObject(proof.package_instance_id(),symbol,
        pin.expected_source_sha256,pin.expected_object_sha256,
        pin.target_triple,pin.optimization_profile,std::move(bytes));
}

} // namespace oreslang::package_interface
