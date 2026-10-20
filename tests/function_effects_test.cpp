#include "oreslang/types/FunctionEffects.hpp"
#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <utility>

using namespace oreslang;

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
}

int main() {
    try {
        FunctionEffects declared{};
        declared.bits = static_cast<std::uint32_t>(FunctionEffect::StateRead)
                      | static_cast<std::uint32_t>(FunctionEffect::StateWrite)
                      | static_cast<std::uint32_t>(FunctionEffect::Suspend);
        FunctionEffects read{static_cast<std::uint32_t>(FunctionEffect::StateRead)};
        require(declared.permits(read), "declared effects must permit read");
        require(!read.permits(declared), "read must not permit broader effects");
        require(declared.united({}).permits(declared), "union with empty effects");
        require(FunctionEffects::parse("state.write") == FunctionEffect::StateWrite,
                "state.write must parse");
        require(FunctionEffects::parse("raise") != FunctionEffects::parse("throw"),
                "raise and throw must be distinct");
        require(FunctionEffects::parse("panic") != FunctionEffects::parse("throw"),
                "panic and throw must be distinct");
        require(FunctionEffects::parse("panic") != FunctionEffects::parse("raise"),
                "panic and raise must be distinct");
        // Exhaustive spelling and one-hot flag coverage: adding or aliasing an
        // effect must require updating this reviewed table.
        const std::array<std::pair<std::string_view, FunctionEffect>, 10> known{{
            {"state.read", FunctionEffect::StateRead},
            {"state.write", FunctionEffect::StateWrite},
            {"suspend", FunctionEffect::Suspend},
            {"io", FunctionEffect::Io},
            {"alloc", FunctionEffect::Alloc},
            {"block", FunctionEffect::Block},
            {"ffi", FunctionEffect::Ffi},
            {"throw", FunctionEffect::Throw},
            {"raise", FunctionEffect::Raise},
            {"panic", FunctionEffect::Panic},
        }};
        std::uint32_t seen = 0;
        for (const auto& [name, effect] : known) {
            const auto bit = static_cast<std::uint32_t>(effect);
            require(bit != 0 && (bit & (bit - 1u)) == 0,
                    "effect flag must be one-hot");
            require((seen & bit) == 0, "effect flags must never alias");
            require(FunctionEffects::parse(name) == effect,
                    "effect spelling must parse to its exact flag");
            const FunctionEffects only{bit};
            require(only.has(effect), "effect bitset must contain its own flag");
            require(!FunctionEffects{}.has(effect),
                    "empty bitset must not contain an effect");
            require(declared.united(only).permits(only),
                    "effect union must include its right-hand operand");
            require(only.permits({}), "every effect set must permit pure calls");
            seen |= bit;
        }
        require(seen == ((1u << known.size()) - 1u),
                "effect flags must form the reviewed contiguous bit range");

        // Exhaust all 2^10 by 2^10 ordered permission pairs, including
        // empty, singleton, composite, and disjoint effect sets. Deliberately
        // run as ordinary executable code: assert() disappears under NDEBUG.
        constexpr std::uint32_t all_effects = (1u << known.size()) - 1u;
        for (std::uint32_t lhs = 0; lhs <= all_effects; ++lhs) {
            const FunctionEffects a{lhs};
            require(a.permits(a), "every effect set must permit itself");
            require(a.permits({}), "every effect set must permit pure effects");
            require(a.united({}).bits == lhs, "union with empty set is identity");
            for (const auto& [name, effect] : known) {
                (void)name;
                const auto bit = static_cast<std::uint32_t>(effect);
                require(a.has(effect) == ((lhs & bit) != 0),
                        "effect membership must match independent bit mask");
            }
            for (std::uint32_t rhs = 0; rhs <= all_effects; ++rhs) {
                const FunctionEffects b{rhs};
                require(a.permits(b) == ((lhs & rhs) == rhs),
                        "permits must implement exact subset relation");
                require(a.united(b).bits == (lhs | rhs),
                        "effect union must match independent bitwise OR");
                require(a.united(b).permits(a) && a.united(b).permits(b),
                        "union must permit both operands");
                require(a.united(b).bits == b.united(a).bits,
                        "effect union must be commutative");
            }
        }

        // Deliberately forged bitsets must never be approved. With the old
        // subset-only permits() check, equal unknown-bit masks were accepted,
        // including as the declared actor-state "using" effects.
        constexpr std::uint32_t unknown_high = 1u << 31;
        constexpr std::uint32_t unknown_next = 1u << 10;
        constexpr std::uint32_t read_bit =
            static_cast<std::uint32_t>(FunctionEffect::StateRead);
        constexpr std::uint32_t write_bit =
            static_cast<std::uint32_t>(FunctionEffect::StateWrite);
        require(FunctionEffects::KnownBits == all_effects,
                "reviewed effect table must match the runtime allowlist");
        require(FunctionEffects{}.valid(), "pure effects must be valid");
        require(FunctionEffects{all_effects}.valid(),
                "all recognized effects must be valid");
        for (const std::uint32_t invalid_bit :
             std::array<std::uint32_t, 4>{{unknown_next, unknown_high,
                                           read_bit | unknown_high,
                                           0xFFFFFFFFu}}) {
            const FunctionEffects poisoned{invalid_bit};
            require(!poisoned.valid(), "unrecognized effect bitset must be invalid");
            require(!poisoned.permits({}), "invalid declared effects cannot permit pure");
            require(!poisoned.permits(poisoned),
                    "unknown bits cannot authorize themselves");
            require(!FunctionEffects{all_effects}.permits(poisoned),
                    "recognized effects cannot authorize unknown bits");
            require(!poisoned.united({}).valid(),
                    "union with pure cannot silently cleanse an invalid mask");
            require(!poisoned.has(FunctionEffect::StateRead),
                    "invalid effect set cannot grant a recognized permission");
        }
        for (const std::uint32_t invalid_flag :
             std::array<std::uint32_t, 6>{{0u, read_bit | write_bit,
                                           unknown_next, unknown_high,
                                           0xFFFFFFFFu, read_bit | unknown_high}}) {
            const auto forged = static_cast<FunctionEffect>(invalid_flag);
            require(!FunctionEffects{all_effects}.has(forged),
                    "zero, composite, or unknown cast flag must not match");
        }
        require(!FunctionEffects{read_bit}.has(
                    static_cast<FunctionEffect>(read_bit | write_bit)),
                "composite flag cannot accidentally match a partial subset");
        // The known-domain tests above exhaust all valid rows.
        // Extra adversarial values exercise the opaque public ABI boundary.

        const std::array<std::string_view, 13> invalid{{
            std::string_view{}, "unknown", "state", "state.READ", "state.read ",
            " state.read", "state-read", "suspend\n", "PANIC", "panic.",
            "throw/raise", std::string_view{"panic\0extra", 11},
            std::string_view{"\0panic", 6}
        }};
        for (std::string_view spelling : invalid) {
            bool rejected = false;
            try {
                (void) FunctionEffects::parse(spelling);
            } catch (const std::invalid_argument&) {
                rejected = true;
            }
            require(rejected, "unknown or noncanonical effect spelling must fail");
        }
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << "function effects conformance: " << failure.what() << '\n';
        return 1;
    }
}
