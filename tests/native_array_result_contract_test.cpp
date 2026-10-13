#include "oreslang/types/NativeArrayResultContract.hpp"

#include <array>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <type_traits>

using namespace oreslang::native::adt;

#define CHECK(cond) do { if (!(cond)) return __LINE__; } while (false)

static_assert(!std::is_convertible_v<AppendResult, int>);
static_assert(!std::is_convertible_v<AppendResult, bool>);
constexpr std::array<MatchArm, 3> full = {{
    {Pattern::Ok}, {Pattern::ErrLimit}, {Pattern::ErrNoMemory}
}};
constexpr std::array<MatchArm, 2> grouped = {{
    {Pattern::ErrAny}, {Pattern::Ok}
}};
constexpr std::array<MatchArm, 2> partial = {{
    {Pattern::Ok}, {Pattern::ErrLimit}
}};
static_assert(prove_array_append_match(full.data(), full.size())
        == MatchProof::Exhaustive);
static_assert(prove_array_append_match(grouped.data(), grouped.size())
        == MatchProof::Exhaustive);
static_assert(prove_array_append_match(partial.data(), partial.size())
        == MatchProof::NonExhaustive);

int main() {
    AppendResult value{};
    value.variant = ResultVariant::Err;
    value.error = ArrayError::NoMemory;
    CHECK(decode_checked_append(0, value));
    CHECK(value.variant == ResultVariant::Ok);
    CHECK(decode_checked_append(1, value));
    CHECK(value.variant == ResultVariant::Err);
    CHECK(value.error == ArrayError::Limit);
    CHECK(decode_checked_append(2, value));
    CHECK(value.variant == ResultVariant::Err);
    CHECK(value.error == ArrayError::NoMemory);

    // Exhaustively test the entire low unsigned-byte domain and high
    // adversarial values. For invalid status, output is bitwise unchanged.
    for (std::uint32_t code = 3; code != 256; ++code) {
        AppendResult sentinel{ResultVariant::Err, ArrayError::NoMemory};
        CHECK(!decode_checked_append(code, sentinel));
        CHECK(sentinel.variant == ResultVariant::Err);
        CHECK(sentinel.error == ArrayError::NoMemory);
    }
    for (const std::uint32_t code : {
            std::uint32_t{0x7fffffff}, std::uint32_t{0x80000000},
            std::uint32_t{0xffffffff}}) {
        AppendResult sentinel{ResultVariant::Err, ArrayError::Limit};
        CHECK(!decode_checked_append(code, sentinel));
        CHECK(sentinel.variant == ResultVariant::Err);
        CHECK(sentinel.error == ArrayError::Limit);
    }

    constexpr MatchArm wildcard[] = {
        {Pattern::Ok}, {Pattern::Wildcard}
    };
    CHECK(prove_array_append_match(wildcard, 2) == MatchProof::Exhaustive);
    constexpr MatchArm only_error[] = {{Pattern::ErrAny}};
    CHECK(prove_array_append_match(only_error, 1) == MatchProof::NonExhaustive);
    constexpr MatchArm duplicate[] = {{Pattern::Ok}, {Pattern::Ok}};
    CHECK(prove_array_append_match(duplicate, 2) == MatchProof::Overlapping);
    constexpr MatchArm shadowed[] = {
        {Pattern::ErrAny}, {Pattern::ErrLimit}
    };
    CHECK(prove_array_append_match(shadowed, 2) == MatchProof::Overlapping);
    constexpr MatchArm dead_wildcard[] = {
        {Pattern::Ok}, {Pattern::ErrLimit},
        {Pattern::ErrNoMemory}, {Pattern::Wildcard}
    };
    CHECK(prove_array_append_match(dead_wildcard, 4) == MatchProof::DeadArm);
    constexpr MatchArm early_wildcard[] = {
        {Pattern::Wildcard}, {Pattern::Ok}
    };
    CHECK(prove_array_append_match(early_wildcard, 2)
          == MatchProof::InvalidPattern);
    constexpr MatchArm guard[] = {
        {Pattern::Ok}, {Pattern::ErrAny, true}
    };
    CHECK(prove_array_append_match(guard, 2)
          == MatchProof::GuardNeedsProof);
    constexpr MatchArm forged[] = {
        {static_cast<Pattern>(0xff)}
    };
    CHECK(prove_array_append_match(forged, 1)
          == MatchProof::InvalidPattern);
    CHECK(prove_array_append_match(nullptr, 0) == MatchProof::BadArity);
    CHECK(prove_array_append_match(nullptr, 1) == MatchProof::BadArity);

    // Enumerate all 5^3 three-arm combinations: every proof must either
    // establish exactly all three inhabitants, or reject false coverage.
    for (unsigned a = 0; a < 5; ++a)
        for (unsigned b = 0; b < 5; ++b)
            for (unsigned c = 0; c < 5; ++c) {
                const MatchArm cases[] = {
                    {static_cast<Pattern>(a)}, {static_cast<Pattern>(b)},
                    {static_cast<Pattern>(c)}
                };
                const MatchProof proof = prove_array_append_match(cases, 3);
                if (proof != MatchProof::Exhaustive) continue;
                // Independently enumerate all values and require exactly
                // one matching arm: avoid unsound exhaustiveness claims.
                for (unsigned v = 0; v < 3; ++v) {
                    unsigned matches = 0;
                    bool covered_by_explicit = false;
                    for (unsigned k = 0; k < 3; ++k) {
                        const Pattern pattern = cases[k].pattern;
                        const bool explicit_match =
                            (pattern == Pattern::Ok && v == 0) ||
                            (pattern == Pattern::ErrLimit && v == 1) ||
                            (pattern == Pattern::ErrNoMemory && v == 2) ||
                            (pattern == Pattern::ErrAny && v != 0);
                        // Final wildcard means the *complement* of all
                        // preceding explicit arms, not another full-domain
                        // overlapping pattern.
                        const bool matched = explicit_match ||
                            (pattern == Pattern::Wildcard && !covered_by_explicit);
                        if (matched) ++matches;
                        covered_by_explicit |= explicit_match;
                    }
                    CHECK(matches == 1);
                }
            }
    return 0;
}
