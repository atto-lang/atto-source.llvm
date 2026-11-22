// HOST_ONLY: tests policy preflight only, NOT hardware quote verification.
#include "oreslang/runtime/ConfidentialAttestationPreflight.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

using namespace oreslang::runtime::vm::confidential;

namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { ++failures; std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); } } while (0)

Requirements requirements() {
    Requirements r;
    r.provider = Provider::AmdSevSnp;
    r.measurement_sha256.fill(7);
    r.min_tcb = 5;
    r.max_evidence_age_ms = 100;
    return r;
}
std::array<std::uint8_t, 32> nonce() {
    std::array<std::uint8_t, 32> n{};
    n.fill(9);
    return n; // fixed for HOST_ONLY test; real challenge requires OS CSPRNG
}
UntrustedClaims claims() {
    UntrustedClaims c;
    c.provider = Provider::AmdSevSnp;
    c.measurement_sha256.assign(32, 7);
    c.challenge_nonce.assign(32, 9);
    c.issued_at_ms = 1000;
    c.tcb = 5;
    c.opaque_quote = {1}; // a fake quote cannot authorize anything
    return c;
}
void one_shot_claims_are_not_hardware_attestation() {
    Session s(requirements());
    CHECK(s.check_untrusted_claims(claims(), 1000) == Verdict::RejectedState);
    CHECK(s.issue_challenge(nonce(), 1000, 50));
    CHECK(s.state() == State::ChallengeIssued);
    CHECK(s.check_untrusted_claims(claims(), 1000) == Verdict::UnverifiedClaimsMatch);
    CHECK(s.state() == State::UnverifiedClaimsMatch);
    CHECK(s.check_untrusted_claims(claims(), 1000) == Verdict::RejectedState);
    CHECK(!s.issue_challenge(nonce(), 1001, 50));
}
void negative_matrix() {
    for (int which = 0; which < 11; ++which) {
        Session s(requirements());
        CHECK(s.issue_challenge(nonce(), 1000, 50));
        auto c = claims();
        std::uint64_t now = 1001;
        switch (which) {
            case 0: c.provider = Provider::IntelTdx; break;
            case 1: c.challenge_nonce[0] ^= 1; break;
            case 2: c.measurement_sha256[0] ^= 1; break;
            case 3: c.tcb = 4; break;
            case 4: c.debug_enabled = true; break;
            case 5: c.revoked = true; break;
            case 6: c.opaque_quote.clear(); break;
            case 7: c.issued_at_ms = 999; break;
            case 8: now = 1051; break;
            case 9: c.challenge_nonce.pop_back(); break;
            case 10: c.issued_at_ms = 1100; break;
        }
        CHECK(s.check_untrusted_claims(c, now) != Verdict::UnverifiedClaimsMatch);
        CHECK(s.state() == State::Rejected);
        CHECK(s.check_untrusted_claims(claims(), 1001) == Verdict::RejectedState);
    }
}
void os_challenge_is_kernel_generated_or_fails_closed() {
    Session s(requirements());
    std::array<std::uint8_t, 32> challenge{};
#if defined(__linux__)
    CHECK(s.issue_os_challenge(1000, 50, challenge));
    CHECK(s.state() == State::ChallengeIssued);
    auto c = claims();
    c.challenge_nonce.assign(challenge.begin(), challenge.end());
    CHECK(s.check_untrusted_claims(c, 1000) == Verdict::UnverifiedClaimsMatch);
#else
    CHECK(!s.issue_os_challenge(1000, 50, challenge));
    CHECK(s.state() == State::Created);
#endif
}
void invalid_requirements_and_entropy() {
    auto r = requirements();
    r.measurement_sha256.fill(0);
    Session bad(r);
    CHECK(!bad.issue_challenge(nonce(), 1000, 50));
    Session good(requirements());
    std::array<std::uint8_t, 32> zero{};
    CHECK(!good.issue_challenge(zero, 1000, 50));
    CHECK(!good.issue_challenge(nonce(), 1000, 0));
    CHECK(good.issue_challenge(nonce(), 1000, 50));
}
}

int main() {
    one_shot_claims_are_not_hardware_attestation();
    negative_matrix();
    invalid_requirements_and_entropy();
    os_challenge_is_kernel_generated_or_fails_closed();
    if (failures) std::fprintf(stderr, "HOST_ONLY: %d failures\n", failures);
    else std::puts("HOST_ONLY: preflight rejection tests passed; NO hardware attestation");
    return failures ? 1 : 0;
}
