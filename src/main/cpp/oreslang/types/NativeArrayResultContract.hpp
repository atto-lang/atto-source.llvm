#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace oreslang::native::adt {

// This is a deliberately CLOSED schema for Result<void, ArrayError>,
// not a generic Result<T,E>, a guest integer or an exception object.
// Its tag identities are nominal, not inferred from payload field shapes.
// Wire format and Java/GraalVM guest representation are separate gates.
enum class ArrayError : std::uint8_t { Limit = 1, NoMemory = 2 };
enum class ResultVariant : std::uint8_t { Ok = 0, Err = 1 };

struct AppendResult final {
    ResultVariant variant = ResultVariant::Ok;
    ArrayError error = ArrayError::Limit; // ignored for Ok; never observed
};
static_assert(std::is_trivially_copyable_v<AppendResult>);
static_assert(!std::is_convertible_v<AppendResult, std::uint32_t>);
static_assert(!std::is_convertible_v<AppendResult, std::int64_t>);

// Lossless decoding from trusted NativeIntArrayRegion status codes.
// Do NOT mask/truncate the input; unexpected values are runtime invariants.
// A false return means "terminate after owner cleanup", NOT Err(ArrayError).
// This function makes no allocations, and does not modify output on failure.
[[nodiscard]] constexpr bool decode_checked_append(
        std::uint32_t status, AppendResult& output) noexcept {
    AppendResult candidate{};
    switch (status) {
        case 0: candidate.variant = ResultVariant::Ok; break;
        case 1:
            candidate.variant = ResultVariant::Err;
            candidate.error = ArrayError::Limit;
            break;
        case 2:
            candidate.variant = ResultVariant::Err;
            candidate.error = ArrayError::NoMemory;
            break;
        default: return false;
    }
    output = candidate;
    return true;
}

enum class Pattern : std::uint8_t {
    Ok,
    ErrLimit,
    ErrNoMemory,
    ErrAny,
    Wildcard,
};
struct MatchArm final {
    Pattern pattern;
    bool guarded = false;
};
enum class MatchProof : std::uint8_t {
    Exhaustive, NonExhaustive, Overlapping, DeadArm,
    GuardNeedsProof, InvalidPattern, BadArity
};

// Finite closed domain: exactly Ok, Err(Limit), Err(NoMemory).
// A payload-specific Err(Limit) does NOT cover every Err.
// Overlap is rejected for exclusive match. An unguarded final wildcard
// consumes only the complement of preceding explicit cases.
// A guarded arm contributes no universal coverage; until predicate proof
// exists, guarded exclusive matches are deliberately rejected.
[[nodiscard]] constexpr MatchProof prove_array_append_match(
        const MatchArm* arms, std::size_t count) noexcept {
    constexpr std::uint8_t all = 0x07;
    if (count == 0 || count > 4 || arms == nullptr)
        return MatchProof::BadArity;
    std::uint8_t covered = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const MatchArm arm = arms[i];
        if (arm.guarded) return MatchProof::GuardNeedsProof;
        std::uint8_t bits = 0;
        switch (arm.pattern) {
            case Pattern::Ok: bits = 0x01; break;
            case Pattern::ErrLimit: bits = 0x02; break;
            case Pattern::ErrNoMemory: bits = 0x04; break;
            case Pattern::ErrAny: bits = 0x06; break;
            case Pattern::Wildcard:
                if (i + 1 != count) return MatchProof::InvalidPattern;
                bits = all & static_cast<std::uint8_t>(~covered);
                break;
            default: return MatchProof::InvalidPattern;
        }
        if (bits == 0) return MatchProof::DeadArm;
        if ((covered & bits) != 0) return MatchProof::Overlapping;
        covered |= bits;
    }
    return covered == all ? MatchProof::Exhaustive
                          : MatchProof::NonExhaustive;
}

} // namespace oreslang::native::adt
