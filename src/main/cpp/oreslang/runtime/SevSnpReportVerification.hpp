#pragma once
// AMD SEV-SNP raw report inspection + *signature-only* crypto primitive.
// This intentionally has NO path to grant access, attest a workload or release
// secrets. A trusted AMD root -> ASK/ASVK -> VCEK/VLEK endorsement chain, AMD
// TCB extensions, chip ID binding and current CRLs must be verified elsewhere.
// Source: AMD publication 56860, SNP ATTESTATION_REPORT (1184 bytes).
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>
#include <cstring>
#include <openssl/bn.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>

namespace oreslang::runtime::vm::confidential::snp {
constexpr std::size_t kReportBytes = 0x4a0;
constexpr std::size_t kSignedBytes = 0x2a0;
constexpr std::size_t kReportDataOffset = 0x50;
constexpr std::size_t kMeasurementOffset = 0x90;
constexpr std::size_t kSignatureROffset = 0x2a0;
constexpr std::size_t kSignatureSOffset = 0x2e8;
constexpr std::size_t kSignatureElementBytes = 72;

enum class Signer : std::uint8_t { Vcek = 0, Vlek = 1 };
enum class Result : std::uint8_t {
    UnverifiedPolicyMatch, RejectedSize, RejectedVersion, RejectedAlgorithm,
    RejectedSigner, RejectedVmpl, RejectedDebug, RejectedReportData,
    RejectedMeasurement, RejectedTcb, RejectedPolicy
};
struct TcbFloor { std::uint8_t bootloader = 0, tee = 0, snp = 0, microcode = 0; };
struct Policy {
    std::array<std::uint8_t, 64> expected_report_data{}; // bound nonce + ephemeral TLS key
    std::array<std::uint8_t, 48> expected_measurement{}; // SHA-384 launch digest
    TcbFloor minimum_tcb{};
    Signer signer = Signer::Vcek;
    std::uint32_t min_report_version = 2;
    std::uint32_t max_report_version = 5;
};
inline std::uint32_t le32(const std::uint8_t* b) noexcept {
    return std::uint32_t(b[0]) | (std::uint32_t(b[1]) << 8) |
           (std::uint32_t(b[2]) << 16) | (std::uint32_t(b[3]) << 24);
}
inline std::uint64_t le64(const std::uint8_t* b) noexcept {
    return std::uint64_t(le32(b)) | (std::uint64_t(le32(b + 4)) << 32);
}
inline bool equal_bytes(const std::uint8_t* lhs, const std::uint8_t* rhs,
                        std::size_t n) noexcept {
    std::uint8_t diff = 0;
    for (std::size_t i = 0; i < n; ++i) diff |= lhs[i] ^ rhs[i];
    return diff == 0;
}
inline bool nonzero(const std::uint8_t* bytes, std::size_t n) noexcept {
    std::uint8_t bits = 0;
    for (std::size_t i = 0; i < n; ++i) bits |= bytes[i];
    return bits != 0;
}
inline Result inspect_unverified_report(const std::uint8_t* report,
                                        std::size_t size,
                                        const Policy& policy) noexcept {
    if (!report || size != kReportBytes) return Result::RejectedSize;
    if (policy.min_report_version < 2 || policy.max_report_version > 5 ||
        policy.min_report_version > policy.max_report_version ||
        !nonzero(policy.expected_report_data.data(), 64) ||
        !nonzero(policy.expected_measurement.data(), 48) ||
        (policy.minimum_tcb.bootloader == 0 && policy.minimum_tcb.tee == 0 &&
         policy.minimum_tcb.snp == 0 && policy.minimum_tcb.microcode == 0))
        return Result::RejectedPolicy;
    const auto version = le32(report);
    if (version < policy.min_report_version || version > policy.max_report_version)
        return Result::RejectedVersion;
    // Signature algorithm 1 = ECDSA P-384 with SHA-384; VMPL must be zero.
    if (le32(report + 0x34) != 1) return Result::RejectedAlgorithm;
    if (le32(report + 0x30) != 0) return Result::RejectedVmpl;
    const auto signer = le32(report + 0x48);
    if ((signer & 0x3u) != static_cast<std::uint32_t>(policy.signer) ||
        (signer & ~0xffu) != 0) return Result::RejectedSigner;
    if ((le64(report + 0x08) & (std::uint64_t{1} << 19)) != 0)
        return Result::RejectedDebug;
    if (!equal_bytes(report + kReportDataOffset, policy.expected_report_data.data(), 64))
        return Result::RejectedReportData;
    if (!equal_bytes(report + kMeasurementOffset, policy.expected_measurement.data(), 48))
        return Result::RejectedMeasurement;
    const auto* tcb = report + 0x180;
    if (tcb[0] < policy.minimum_tcb.bootloader || tcb[1] < policy.minimum_tcb.tee ||
        tcb[6] < policy.minimum_tcb.snp || tcb[7] < policy.minimum_tcb.microcode)
        return Result::RejectedTcb;
    // No vendor quote-signature, certificate or report-data channel binding
    // has been cryptographically verified by inspect_unverified_report.
    return Result::UnverifiedPolicyMatch;
}

// Deterministic host-side REPORT_DATA: SHA512(domain || nonce32 ||
// image_identity32 || uint32_be(spki_len) || ephemeral_tls_spki_der).
// The caller must authenticate the image identity and generate the nonce and
// SPKI key pair in a trusted context. This is *only* a binding primitive;
// not a complete authenticated transport, endorsement verifier or key broker.
inline bool compute_bound_report_data(const std::array<std::uint8_t, 32>& nonce,
                                      const std::array<std::uint8_t, 32>& image_identity,
                                      const std::uint8_t* spki_der, std::size_t spki_bytes,
                                      std::array<std::uint8_t, 64>& out) noexcept {
    out.fill(0);
    if (!nonzero(nonce.data(), nonce.size()) ||
        !nonzero(image_identity.data(), image_identity.size()) ||
        !spki_der || spki_bytes == 0 || spki_bytes > 4096) return false;
    constexpr char kDomain[] = "atto-sev-snp-report-data-v1";
    const std::uint8_t len[4] = {
        static_cast<std::uint8_t>((spki_bytes >> 24) & 0xff),
        static_cast<std::uint8_t>((spki_bytes >> 16) & 0xff),
        static_cast<std::uint8_t>((spki_bytes >> 8) & 0xff),
        static_cast<std::uint8_t>(spki_bytes & 0xff)
    };
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return false;
    unsigned int size = 0;
    const bool ok = EVP_DigestInit_ex(ctx, EVP_sha512(), nullptr) == 1 &&
        EVP_DigestUpdate(ctx, kDomain, sizeof(kDomain) - 1) == 1 &&
        EVP_DigestUpdate(ctx, nonce.data(), nonce.size()) == 1 &&
        EVP_DigestUpdate(ctx, image_identity.data(), image_identity.size()) == 1 &&
        EVP_DigestUpdate(ctx, len, sizeof(len)) == 1 &&
        EVP_DigestUpdate(ctx, spki_der, spki_bytes) == 1 &&
        EVP_DigestFinal_ex(ctx, out.data(), &size) == 1 && size == out.size();
    EVP_MD_CTX_free(ctx);
    if (!ok) out.fill(0);
    return ok;
}

// Verifies the SHA384/P384 ECDSA signature over 0x000..0x29F with an ALREADY
// authenticated P-384 endorsement public key. This does NOT authenticate a
// VCEK/VLEK key or verify the AMD certificate chain, CRLs, chip_id or TCB.
// Never use this boolean alone to authorize a workload or secret release.
inline bool verify_signature_only(const std::uint8_t* report, std::size_t size,
                                  EVP_PKEY* key) {
    if (!report || size != kReportBytes || !key ||
        EVP_PKEY_base_id(key) != EVP_PKEY_EC || le32(report + 0x34) != 1) return false;
    char group[64] = {};
    std::size_t group_length = 0;
    if (EVP_PKEY_get_group_name(key, group, sizeof(group), &group_length) != 1 ||
        (std::strcmp(group, "secp384r1") != 0 &&
         std::strcmp(group, "prime384v1") != 0)) return false;
    // Raw ECDSA signature has LE r and s, each allocated 72 bytes; 24 high
    // bytes must be zero so the integers never exceed the 384-bit curve size.
    for (std::size_t i = 48; i < kSignatureElementBytes; ++i)
        if (report[kSignatureROffset+i] || report[kSignatureSOffset+i]) return false;
    for (std::size_t i = kSignatureSOffset + kSignatureElementBytes; i < size; ++i)
        if (report[i]) return false; // reserved section must be zero
    BIGNUM* r = BN_lebin2bn(report + kSignatureROffset, 48, nullptr);
    BIGNUM* s = BN_lebin2bn(report + kSignatureSOffset, 48, nullptr);
    if (!r || !s) { BN_free(r); BN_free(s); return false; }
    ECDSA_SIG* sig = ECDSA_SIG_new();
    if (!sig) { BN_free(r); BN_free(s); return false; }
    if (ECDSA_SIG_set0(sig, r, s) != 1) {
        BN_free(r); BN_free(s); ECDSA_SIG_free(sig); return false;
    }
    const int der_length = i2d_ECDSA_SIG(sig, nullptr);
    std::vector<unsigned char> der(der_length > 0 ? static_cast<std::size_t>(der_length) : 0);
    unsigned char* ptr = der.data();
    const bool encoded = der_length > 0 && i2d_ECDSA_SIG(sig, &ptr) == der_length;
    ECDSA_SIG_free(sig);
    if (!encoded) return false;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return false;
    const bool valid = EVP_DigestVerifyInit(ctx, nullptr, EVP_sha384(), nullptr, key) == 1 &&
        EVP_DigestVerify(ctx, der.data(), der.size(), report, kSignedBytes) == 1;
    EVP_MD_CTX_free(ctx);
    return valid;
}
} // namespace oreslang::runtime::vm::confidential::snp
