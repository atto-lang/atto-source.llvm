// HOST_ONLY: this test deliberately provides no AMD trusted roots.
// A structurally matching report, arbitrary pin, or synthetic certificate
// MUST NOT be convertible into AMD endorsement authentication.
#include "oreslang/runtime/SevSnpVcekEndorsement.hpp"
#include <openssl/evp.h>
#include <array>
#include <cstdio>
#include <vector>
using namespace oreslang::runtime::vm::confidential::snp;
static int failures=0;
#define CHECK(expr) do { if(!(expr)){++failures; std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#expr);} } while(0)

int main() {
    Policy policy;
    policy.signer=Signer::Vcek;
    policy.expected_report_data.fill(9);
    policy.expected_measurement.fill(7);
    policy.minimum_tcb={1,1,1,1};
    std::vector<std::uint8_t> report(kReportBytes,0);
    report[0]=3;
    report[0x34]=1;
    for(std::size_t i=0;i<64;++i) report[kReportDataOffset+i]=9;
    for(std::size_t i=0;i<48;++i) report[kMeasurementOffset+i]=7;
    report[0x180]=1;report[0x181]=1;report[0x186]=1;report[0x187]=1;
    // Unverified policy match alone does not establish AMD provenance.
    CHECK(inspect_unverified_report(report.data(),report.size(),policy)==Result::UnverifiedPolicyMatch);
    OfflineEndorsements empty;
    CHECK(verify_vcek_offline(report.data(),report.size(),policy,empty)==EndorsementStatus::UntrustedRoot);
    CHECK(verify_vcek_offline(report.data(),10,policy,empty)==EndorsementStatus::UnverifiedInput);
    policy.signer=Signer::Vlek;
    CHECK(verify_vcek_offline(report.data(),report.size(),policy,empty)==EndorsementStatus::UnsupportedSigner);
    policy.signer=Signer::Vcek;
    report[0x48]=1;
    CHECK(verify_vcek_offline(report.data(),report.size(),policy,empty)==EndorsementStatus::UnsupportedSigner);
    report[0x48]=0;

    // A verifier must NOT derive the trusted pin from a guest-provided root.
    empty.pinned_ark_der={0x30,0x01,0x00}; // invalid DER, not an AMD root
    empty.expected_ark_der_sha256.fill(7); // incorrect pin
    CHECK(!root_pin_matches(empty));
    CHECK(verify_vcek_offline(report.data(),report.size(),policy,empty)==EndorsementStatus::UntrustedRoot);

    unsigned int digest_len=0;
    CHECK(EVP_Digest(empty.pinned_ark_der.data(),empty.pinned_ark_der.size(),
            empty.expected_ark_der_sha256.data(),&digest_len,EVP_sha256(),nullptr)==1);
    CHECK(digest_len==32);
    CHECK(root_pin_matches(empty)); // only the *byte equality* primitive succeeded
    CHECK(verify_vcek_offline(report.data(),report.size(),policy,empty)==EndorsementStatus::InvalidCertificates);

    // Bad certificate bytes are explicitly refused, not a fallback status.
    CHECK(!parse_certificate(empty.pinned_ark_der));
    CHECK(!parse_crl({1,2,3}));
    std::printf("HOST_ONLY VCEK rejection tests: %d failures; NO AMD root/quote authenticated\n",failures);
    return failures?1:0;
}
