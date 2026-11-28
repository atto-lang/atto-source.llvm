#pragma once
// Atto native confidential-computing CLAIMS preflight — not vendor attestation.
// IMPORTANT: this header cannot authorize any confidential TenantVM or secret release.
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>
#if defined(__linux__)
#include <cerrno>
#include <sys/random.h>
#endif

namespace oreslang::runtime::vm::confidential {

enum class Provider : std::uint8_t { AmdSevSnp, IntelTdx };
enum class State : std::uint8_t { Created, ChallengeIssued, UnverifiedClaimsMatch, Rejected };
enum class Verdict : std::uint8_t {
    UnverifiedClaimsMatch,
    RejectedState, RejectedEvidence, RejectedProvider, RejectedNonce,
    RejectedMeasurement, RejectedTcb, RejectedDebug, RejectedRevoked,
    RejectedFreshness
};

struct Requirements {
    Provider provider = Provider::AmdSevSnp;
    std::array<std::uint8_t, 32> measurement_sha256{};
    std::uint64_t min_tcb = 0;
    std::uint64_t max_evidence_age_ms = 0;
};

// All fields supplied here are untrusted until independently verified by a
// real vendor signature-chain/TCB verifier. The quote is deliberately opaque.
struct UntrustedClaims {
    Provider provider = Provider::AmdSevSnp;
    std::vector<std::uint8_t> measurement_sha256;
    std::vector<std::uint8_t> challenge_nonce;
    std::uint64_t issued_at_ms = 0;
    std::uint64_t tcb = 0;
    bool debug_enabled = false;
    bool revoked = false;
    std::vector<std::uint8_t> opaque_quote;
};

inline bool constant_time_equal(const std::uint8_t* left,
                                const std::uint8_t* right,
                                std::size_t length) noexcept {
    std::uint8_t difference = 0;
    for (std::size_t i = 0; i < length; ++i)
        difference |= static_cast<std::uint8_t>(left[i] ^ right[i]);
    return difference == 0;
}

class Session final {
public:
    explicit Session(Requirements requirements) : requirements_(requirements) {}

    State state() const noexcept { return state_; }

    // The trusted broker must supply a fresh 32-byte OS-CSPRNG nonce. This
    // helper does not generate or attest entropy, and must stay off RT turns.
    bool issue_challenge(const std::array<std::uint8_t, 32>& host_nonce,
                         std::uint64_t issued_at_ms,
                         std::uint64_t ttl_ms) noexcept {
        if (state_ != State::Created || ttl_ms == 0
            || requirements_.min_tcb == 0 || requirements_.max_evidence_age_ms == 0
            || (requirements_.provider != Provider::AmdSevSnp
                && requirements_.provider != Provider::IntelTdx)) return false;
        std::uint8_t nonce_nonzero = 0;
        std::uint8_t measurement_nonzero = 0;
        for (std::size_t i = 0; i < host_nonce.size(); ++i) {
            nonce_nonzero |= host_nonce[i];
            measurement_nonzero |= requirements_.measurement_sha256[i];
        }
        if (!nonce_nonzero || !measurement_nonzero) return false;
        nonce_ = host_nonce;
        issued_at_ms_ = issued_at_ms;
        ttl_ms_ = ttl_ms;
        state_ = State::ChallengeIssued;
        return true;
    }

    // Mint a challenge directly from Linux's kernel CSPRNG. On unsupported
    // platforms or entropy failure, do not issue a challenge or fall back
    // to deterministic randomness. This runs only on the trusted host,
    // never on a real-time actor turn.
    bool issue_os_challenge(std::uint64_t issued_at_ms, std::uint64_t ttl_ms,
                            std::array<std::uint8_t, 32>& emitted) noexcept {
        emitted.fill(0);
#if defined(__linux__)
        std::size_t done = 0;
        while (done < emitted.size()) {
            const auto n = ::getrandom(emitted.data() + done, emitted.size() - done, 0);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { emitted.fill(0); return false; }
            done += static_cast<std::size_t>(n);
        }
        if (!issue_challenge(emitted, issued_at_ms, ttl_ms)) {
            emitted.fill(0);
            return false;
        }
        return true;
#else
        (void) issued_at_ms;
        (void) ttl_ms;
        return false;
#endif
    }

    // A single call consumes the challenge on success OR failure.
    // UnverifiedClaimsMatch is a policy-only observation, NEVER a proof.
    Verdict check_untrusted_claims(const UntrustedClaims& claims,
                                   std::uint64_t now_ms) noexcept {
        if (state_ != State::ChallengeIssued) return Verdict::RejectedState;
        state_ = State::Rejected;
        if (claims.opaque_quote.empty() || claims.measurement_sha256.size() != 32
            || claims.challenge_nonce.size() != 32)
            return Verdict::RejectedEvidence;
        if (claims.provider != requirements_.provider) return Verdict::RejectedProvider;
        if (!constant_time_equal(nonce_.data(), claims.challenge_nonce.data(), nonce_.size()))
            return Verdict::RejectedNonce;
        if (!constant_time_equal(requirements_.measurement_sha256.data(),
                                 claims.measurement_sha256.data(), nonce_.size()))
            return Verdict::RejectedMeasurement;
        if (claims.debug_enabled) return Verdict::RejectedDebug;
        if (claims.revoked) return Verdict::RejectedRevoked;
        if (claims.tcb < requirements_.min_tcb) return Verdict::RejectedTcb;
        // Unsigned subtraction is performed only after checking order.
        if (now_ms < issued_at_ms_ || now_ms - issued_at_ms_ > ttl_ms_
            || claims.issued_at_ms < issued_at_ms_
            || claims.issued_at_ms > now_ms
            || now_ms - claims.issued_at_ms > requirements_.max_evidence_age_ms)
            return Verdict::RejectedFreshness;
        state_ = State::UnverifiedClaimsMatch;
        return Verdict::UnverifiedClaimsMatch;
    }

private:
    Requirements requirements_;
    State state_ = State::Created;
    std::array<std::uint8_t, 32> nonce_{};
    std::uint64_t issued_at_ms_ = 0;
    std::uint64_t ttl_ms_ = 0;
};

} // namespace oreslang::runtime::vm::confidential
