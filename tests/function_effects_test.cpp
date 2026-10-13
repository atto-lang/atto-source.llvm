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

        const std::array<std::string_view, 13> invalid{{
            std::string_view{}, "unknown", "state", "state.READ", "state.read ",
            " state.read", "state-read", "suspend\\n", "PANIC", "panic.",
            "throw/raise", std::string_view{"panic\\0extra", 11},
            std::string_view{"\\0panic", 6}
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
