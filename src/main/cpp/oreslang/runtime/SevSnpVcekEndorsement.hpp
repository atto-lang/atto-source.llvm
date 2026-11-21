#pragma once
// Offline SEV-SNP VCEK endorsement verifier, with explicit trusted-host root
// pinning and required DER CRL. Host-only; not an admission/key-release API.
// VLEK and Turin structVersion=1 are deliberately UNSUPPORTED in this version.
#include "oreslang/runtime/SevSnpReportVerification.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>
#include <openssl/asn1.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>
#include <openssl/x509v3.h>

namespace oreslang::runtime::vm::confidential::snp {

enum class EndorsementStatus : std::uint8_t {
    UnverifiedInput, UnsupportedSigner, UntrustedRoot, InvalidCertificates,
    MissingRevocationEvidence, CertificateChainRejected, ReportPolicyRejected,
    TcbMismatch, ChipIdMismatch, ReportSignatureRejected,
    // This is a successful offline CRYPTO CHECK, *not* an admission token.
    EndorsementCryptographyValid
};
struct OfflineEndorsements {
    std::vector<std::uint8_t> pinned_ark_der;
    std::vector<std::uint8_t> ask_der;
    std::vector<std::uint8_t> vcek_der;
    std::vector<std::uint8_t> issuer_crl_der;
    // Separate ARK-issued CRL for ASK revocation (leaf CRL alone is NOT enough).
    std::vector<std::uint8_t> root_issuer_crl_der;
    // SHA256 of canonical AMD-published ARK DER, pinned and provisioned by a
    // trusted deployment authority, NEVER supplied by a guest/report.
    std::array<std::uint8_t, 32> expected_ark_der_sha256{};
};
template <typename T, void(*Free)(T*)> struct ObjectDeleter {
    void operator()(T* x) const noexcept { if (x) Free(x); }
};
using CertPtr = std::unique_ptr<X509, ObjectDeleter<X509, X509_free>>;
using CrlPtr = std::unique_ptr<X509_CRL, ObjectDeleter<X509_CRL, X509_CRL_free>>;
using StorePtr = std::unique_ptr<X509_STORE, ObjectDeleter<X509_STORE, X509_STORE_free>>;
using StoreContextPtr = std::unique_ptr<X509_STORE_CTX,
    ObjectDeleter<X509_STORE_CTX, X509_STORE_CTX_free>>;
using KeyPtr = std::unique_ptr<EVP_PKEY, ObjectDeleter<EVP_PKEY, EVP_PKEY_free>>;

inline CertPtr parse_certificate(const std::vector<std::uint8_t>& bytes) {
    if (bytes.empty() || bytes.size() > 65536) return CertPtr(nullptr);
    const unsigned char* p = bytes.data();
    CertPtr cert(d2i_X509(nullptr, &p, static_cast<long>(bytes.size())));
    if (!cert || p != bytes.data() + bytes.size()) return CertPtr(nullptr);
    return cert;
}
inline CrlPtr parse_crl(const std::vector<std::uint8_t>& bytes) {
    if (bytes.empty() || bytes.size() > 4*1024*1024) return CrlPtr(nullptr);
    const unsigned char* p = bytes.data();
    CrlPtr crl(d2i_X509_CRL(nullptr, &p, static_cast<long>(bytes.size())));
    if (!crl || p != bytes.data() + bytes.size()) return CrlPtr(nullptr);
    return crl;
}
inline bool root_pin_matches(const OfflineEndorsements& endorsements) noexcept {
    std::array<unsigned char, 32> digest{};
    if (endorsements.pinned_ark_der.empty() ||
        endorsements.pinned_ark_der.size() > 65536 ||
        !nonzero(endorsements.expected_ark_der_sha256.data(), 32)) return false;
    unsigned int size = 0;
    return EVP_Digest(endorsements.pinned_ark_der.data(),
                      endorsements.pinned_ark_der.size(), digest.data(),
                      &size, EVP_sha256(), nullptr) == 1 && size == digest.size() &&
        CRYPTO_memcmp(digest.data(),
                      endorsements.expected_ark_der_sha256.data(), digest.size()) == 0;
}
inline bool extension_integer_matches(X509* cert, const char* oid, unsigned expected) {
    ASN1_OBJECT* obj = OBJ_txt2obj(oid, 1);
    if (!obj) return false;
    const int index = X509_get_ext_by_OBJ(cert, obj, -1);
    const bool unique = index >= 0 && X509_get_ext_by_OBJ(cert, obj, index) == -1;
    ASN1_OBJECT_free(obj);
    if (!unique) return false;
    const ASN1_OCTET_STRING* ext = X509_EXTENSION_get_data(X509_get_ext(cert, index));
    if (!ext || ext->length < 3 || ext->length > 16) return false;
    const unsigned char* p = ext->data;
    const auto* end = p + ext->length;
    ASN1_INTEGER* raw = d2i_ASN1_INTEGER(nullptr, &p, ext->length);
    if (!raw) return false;
    const auto value = ASN1_INTEGER_get(raw);
    ASN1_INTEGER_free(raw);
    return p == end && value >= 0 && value <= 255 &&
        static_cast<unsigned>(value) == expected;
}
inline bool vcek_chip_id_matches(X509* vcek,
                                 const std::uint8_t* report, std::size_t size) {
    if (!report || size != kReportBytes) return false;
    ASN1_OBJECT* oid = OBJ_txt2obj("1.3.6.1.4.1.3704.1.4", 1);
    if (!oid) return false;
    const int index = X509_get_ext_by_OBJ(vcek, oid, -1);
    const bool unique = index >= 0 && X509_get_ext_by_OBJ(vcek, oid, index) == -1;
    ASN1_OBJECT_free(oid);
    if (!unique) return false;
    const ASN1_OCTET_STRING* ext = X509_EXTENSION_get_data(X509_get_ext(vcek, index));
    if (!ext || !ext->data) return false;

    // AMD KDS VCEK HWID can be a raw 64-byte extension or an ASN.1
    // OCTET STRING wrapping those bytes. Both represent 64-byte Family19h ID.
    const std::uint8_t* chip = ext->data;
    std::size_t chip_size = static_cast<std::size_t>(ext->length);
    ASN1_OCTET_STRING* wrapped = nullptr;
    if (chip_size != 64) {
        const unsigned char* p = ext->data;
        wrapped = d2i_ASN1_OCTET_STRING(nullptr, &p, ext->length);
        if (!wrapped || p != ext->data + ext->length) {
            ASN1_OCTET_STRING_free(wrapped);
            return false;
        }
        chip = wrapped->data;
        chip_size = static_cast<std::size_t>(wrapped->length);
    }
    const bool match = chip_size == 64 && nonzero(chip, 64) &&
        nonzero(report + 0x1a0, 64) &&
        CRYPTO_memcmp(chip, report + 0x1a0, 64) == 0;
    ASN1_OCTET_STRING_free(wrapped);
    return match;
}
inline bool vcek_tcb_matches(X509* vcek, const std::uint8_t* report,
                             std::size_t size) {
    if (!report || size != kReportBytes) return false;
    const auto* tcb = report + 0x180;
    return extension_integer_matches(vcek, "1.3.6.1.4.1.3704.1.1", 0) &&
        extension_integer_matches(vcek, "1.3.6.1.4.1.3704.1.3.1", tcb[0]) &&
        extension_integer_matches(vcek, "1.3.6.1.4.1.3704.1.3.2", tcb[1]) &&
        extension_integer_matches(vcek, "1.3.6.1.4.1.3704.1.3.3", tcb[6]) &&
        extension_integer_matches(vcek, "1.3.6.1.4.1.3704.1.3.8", tcb[7]);
}

inline EndorsementStatus verify_vcek_offline(
        const std::uint8_t* report, std::size_t report_size,
        const Policy& policy, const OfflineEndorsements& endorsements) {
    if (!report || report_size != kReportBytes) return EndorsementStatus::UnverifiedInput;
    if (policy.signer != Signer::Vcek ||
        (le32(report + 0x48) & 3u) != 0)
        return EndorsementStatus::UnsupportedSigner; // never route VLEK as VCEK
    if (!root_pin_matches(endorsements)) return EndorsementStatus::UntrustedRoot;
    auto root = parse_certificate(endorsements.pinned_ark_der);
    auto ask = parse_certificate(endorsements.ask_der);
    auto vek = parse_certificate(endorsements.vcek_der);
    if (!root || !ask || !vek || X509_check_ca(root.get()) <= 0 ||
        X509_check_ca(ask.get()) <= 0) return EndorsementStatus::InvalidCertificates;
    // OpenSSL treats roots as trusted anchors; pinning alone does not check
    // their expiration or self-signature, so validate those explicitly.
    KeyPtr root_key(X509_get_pubkey(root.get()));
    if (!root_key ||
        X509_NAME_cmp(X509_get_subject_name(root.get()),
                      X509_get_issuer_name(root.get())) != 0 ||
        X509_verify(root.get(), root_key.get()) != 1 ||
        X509_cmp_current_time(X509_get0_notBefore(root.get())) >= 0 ||
        X509_cmp_current_time(X509_get0_notAfter(root.get())) <= 0)
        return EndorsementStatus::InvalidCertificates;
    auto crl = parse_crl(endorsements.issuer_crl_der);
    auto root_crl = parse_crl(endorsements.root_issuer_crl_der);
    if (!crl || !root_crl) return EndorsementStatus::MissingRevocationEvidence;
    StorePtr store(X509_STORE_new());
    StoreContextPtr ctx(X509_STORE_CTX_new());
    if (!store || !ctx ||
        X509_STORE_add_cert(store.get(), root.get()) != 1 ||
        X509_STORE_add_crl(store.get(), crl.get()) != 1 ||
        X509_STORE_add_crl(store.get(), root_crl.get()) != 1 ||
        X509_STORE_set_flags(store.get(), X509_V_FLAG_CRL_CHECK |
                                         X509_V_FLAG_CRL_CHECK_ALL) != 1)
        return EndorsementStatus::CertificateChainRejected;
    STACK_OF(X509)* chain = sk_X509_new_null();
    if (!chain) return EndorsementStatus::CertificateChainRejected;
    const bool stacked = sk_X509_push(chain, ask.get()) == 1;
    const bool initialized = stacked &&
        X509_STORE_CTX_init(ctx.get(), store.get(), vek.get(), chain) == 1;
    const bool chain_valid = initialized && X509_verify_cert(ctx.get()) == 1;
    ctx.reset(); // finish references to the untrusted chain before freeing it
    sk_X509_free(chain); // certificates still owned by unique_ptrs
    if (!chain_valid) return EndorsementStatus::CertificateChainRejected;
    if (inspect_unverified_report(report, report_size, policy) != Result::UnverifiedPolicyMatch)
        return EndorsementStatus::ReportPolicyRejected;
    if (!vcek_tcb_matches(vek.get(), report, report_size))
        return EndorsementStatus::TcbMismatch;
    if (!vcek_chip_id_matches(vek.get(), report, report_size))
        return EndorsementStatus::ChipIdMismatch;
    KeyPtr pkey(X509_get_pubkey(vek.get()));
    if (!pkey || !verify_signature_only(report, report_size, pkey.get()))
        return EndorsementStatus::ReportSignatureRejected;
    return EndorsementStatus::EndorsementCryptographyValid;
}

// Even EndorsementCryptographyValid is not a production-ready TEE verdict:
// this offline subset does not handle all AMD product families, VLEK,
// report-collection freshness, CRL source/freshness pinning, key channel,
// deployment provenance or tenant-generation authorization.
} // namespace oreslang::runtime::vm::confidential::snp
