// HOST_ONLY: Synthetic P-384 signature proves crypto primitive, not AMD endorsement.
#include "oreslang/runtime/SevSnpReportVerification.hpp"
#include <openssl/evp.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>
using namespace oreslang::runtime::vm::confidential::snp;
namespace {
int failures=0;
#define CHECK(x) do { if(!(x)) { ++failures; std::fprintf(stderr,"FAIL %s:%d %s\n", __FILE__,__LINE__,#x); } } while(0)
std::vector<std::uint8_t> report(Policy& policy) {
 std::vector<std::uint8_t> bytes(kReportBytes);
 bytes[0]=3; bytes[0x34]=1; // ABI revision and P384_SHA384
 policy.expected_measurement.fill(7);
 policy.expected_report_data.fill(9);
 policy.minimum_tcb={1,1,1,1};
 std::memcpy(bytes.data()+kMeasurementOffset,policy.expected_measurement.data(),48);
 std::memcpy(bytes.data()+kReportDataOffset,policy.expected_report_data.data(),64);
 bytes[0x180]=1;bytes[0x181]=1;bytes[0x186]=1;bytes[0x187]=1;
 return bytes;
}
struct PkeyDeleter { void operator()(EVP_PKEY* p)const{EVP_PKEY_free(p);} };
using Key=std::unique_ptr<EVP_PKEY,PkeyDeleter>;
Key key(const char* group) {
 EVP_PKEY_CTX* c=EVP_PKEY_CTX_new_from_name(nullptr,"EC",nullptr);
 if(!c)return Key(nullptr);
 EVP_PKEY* k=nullptr;
 bool ok=EVP_PKEY_keygen_init(c)==1 && EVP_PKEY_CTX_set_group_name(c,group)==1 && EVP_PKEY_generate(c,&k)==1;
 EVP_PKEY_CTX_free(c);
 if(!ok){EVP_PKEY_free(k);return Key(nullptr);}return Key(k);
}
void sign(std::vector<std::uint8_t>& bytes, EVP_PKEY* k) {
 EVP_MD_CTX* c=EVP_MD_CTX_new();
 std::size_t len=0;
 CHECK(c && EVP_DigestSignInit(c,nullptr,EVP_sha384(),nullptr,k)==1);
 CHECK(EVP_DigestSign(c,nullptr,&len,bytes.data(),kSignedBytes)==1);
 std::vector<unsigned char> der(len);
 CHECK(EVP_DigestSign(c,der.data(),&len,bytes.data(),kSignedBytes)==1);
 EVP_MD_CTX_free(c);
 const unsigned char* p=der.data();
 ECDSA_SIG* sig=d2i_ECDSA_SIG(nullptr,&p,static_cast<long>(len));
 CHECK(sig!=nullptr);if(!sig)return;
 const BIGNUM *r=nullptr,*s=nullptr;
 ECDSA_SIG_get0(sig,&r,&s);
 CHECK(BN_bn2lebinpad(r,bytes.data()+kSignatureROffset,48)==48);
 CHECK(BN_bn2lebinpad(s,bytes.data()+kSignatureSOffset,48)==48);
 ECDSA_SIG_free(sig);
}
void checks() {
 Policy p; auto b=report(p);
 CHECK(inspect_unverified_report(b.data(),b.size(),p)==Result::UnverifiedPolicyMatch);
 CHECK(inspect_unverified_report(b.data(),b.size()-1,p)==Result::RejectedSize);
 auto wrong=p;wrong.expected_measurement[0]^=1;
 CHECK(inspect_unverified_report(b.data(),b.size(),wrong)==Result::RejectedMeasurement);
 wrong=p;wrong.expected_report_data[0]^=1;
 CHECK(inspect_unverified_report(b.data(),b.size(),wrong)==Result::RejectedReportData);
 auto copy=b;copy[0x0a] |= 0x08; // debug bit 19 (policy byte 2, bit 3)
 CHECK(inspect_unverified_report(copy.data(),copy.size(),p)==Result::RejectedDebug);
 copy=b;copy[0x30]=1;
 CHECK(inspect_unverified_report(copy.data(),copy.size(),p)==Result::RejectedVmpl);
 copy=b;copy[0x34]=0;
 CHECK(inspect_unverified_report(copy.data(),copy.size(),p)==Result::RejectedAlgorithm);
 copy=b;copy[0x48]=1;
 CHECK(inspect_unverified_report(copy.data(),copy.size(),p)==Result::RejectedSigner);
 copy=b;copy[0x187]=0;
 CHECK(inspect_unverified_report(copy.data(),copy.size(),p)==Result::RejectedTcb);
 copy=b;copy[0]=9;
 CHECK(inspect_unverified_report(copy.data(),copy.size(),p)==Result::RejectedVersion);
 wrong=p;wrong.minimum_tcb={};
 CHECK(inspect_unverified_report(b.data(),b.size(),wrong)==Result::RejectedPolicy);
 // Report-data binding uses a verifier-issued nonce, image identity and
 // ephemeral transport key; changing any component must change the digest.
 std::array<std::uint8_t,32> n{}, image{};
 n.fill(9);image.fill(3);
 const std::uint8_t spki[]={0x30,0x03,0x01,0x02,0x03};
 std::array<std::uint8_t,64> bound{}, changed{};
 CHECK(compute_bound_report_data(n,image,spki,sizeof(spki),bound));
 CHECK(nonzero(bound.data(),bound.size()));
 auto changed_nonce=n;changed_nonce[0]^=1;
 CHECK(compute_bound_report_data(changed_nonce,image,spki,sizeof(spki),changed));
 CHECK(!equal_bytes(bound.data(),changed.data(),64));
 auto changed_image=image;changed_image[0]^=1;
 CHECK(compute_bound_report_data(n,changed_image,spki,sizeof(spki),changed));
 CHECK(!equal_bytes(bound.data(),changed.data(),64));
 const std::uint8_t altered[]={0x30,0x03,0x01,0x02,0x04};
 CHECK(compute_bound_report_data(n,image,altered,sizeof(altered),changed));
 CHECK(!equal_bytes(bound.data(),changed.data(),64));
 changed.fill(1);
 CHECK(!compute_bound_report_data(n,image,nullptr,5,changed));
 CHECK(!nonzero(changed.data(),changed.size()));
 auto bound_report=b;
 std::memcpy(bound_report.data()+kReportDataOffset,bound.data(),64);
 auto bound_policy=p;bound_policy.expected_report_data=bound;
 CHECK(inspect_unverified_report(bound_report.data(),bound_report.size(),bound_policy)==Result::UnverifiedPolicyMatch);
 CHECK(inspect_unverified_report(bound_report.data(),bound_report.size(),p)==Result::RejectedReportData);
 auto good=key("secp384r1");auto evil=key("secp384r1");auto small=key("prime256v1");
 CHECK(good&&evil&&small);if(!good||!evil||!small)return;
 sign(b,good.get());
 CHECK(verify_signature_only(b.data(),b.size(),good.get()));
 CHECK(!verify_signature_only(b.data(),b.size(),evil.get()));
 CHECK(!verify_signature_only(b.data(),b.size(),small.get()));
 copy=b;copy[0x99]^=1;
 CHECK(!verify_signature_only(copy.data(),copy.size(),good.get()));
 copy=b;copy[kSignatureROffset]^=1;
 CHECK(!verify_signature_only(copy.data(),copy.size(),good.get()));
 copy=b;copy[kSignatureROffset+55]=1;
 CHECK(!verify_signature_only(copy.data(),copy.size(),good.get()));
 copy=b;copy[0x450]=1;
 CHECK(!verify_signature_only(copy.data(),copy.size(),good.get()));
 copy=b;copy[0x34]=2;
 CHECK(!verify_signature_only(copy.data(),copy.size(),good.get()));
}
}
int main(){checks();std::printf("HOST_ONLY SNP report signature/policy primitive: %d failures; NOT AMD attestation\n",failures);return failures?1:0;}
