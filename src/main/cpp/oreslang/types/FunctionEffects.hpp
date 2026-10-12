#ifndef ORESLANG_FUNCTION_EFFECTS_HPP
#define ORESLANG_FUNCTION_EFFECTS_HPP

#include <cstdint>
#include <stdexcept>
#include <string_view>

namespace oreslang {
enum class FunctionEffect : std::uint32_t {
    StateRead = 1u << 0,
    StateWrite = 1u << 1,
    Suspend = 1u << 2,
    Io = 1u << 3,
    Alloc = 1u << 4,
    Block = 1u << 5,
    Ffi = 1u << 6,
    Throw = 1u << 7,
    Raise = 1u << 8,
    Panic = 1u << 9
};

struct FunctionEffects {
    std::uint32_t bits = 0;
    constexpr bool has(FunctionEffect effect) const {
        return (bits & static_cast<std::uint32_t>(effect)) != 0;
    }
    constexpr bool permits(FunctionEffects inferred) const {
        return (bits & inferred.bits) == inferred.bits;
    }
    constexpr FunctionEffects united(FunctionEffects rhs) const {
        return {bits | rhs.bits};
    }
    static FunctionEffect parse(std::string_view name) {
        if (name == "state.read") return FunctionEffect::StateRead;
        if (name == "state.write") return FunctionEffect::StateWrite;
        if (name == "suspend") return FunctionEffect::Suspend;
        if (name == "io") return FunctionEffect::Io;
        if (name == "alloc") return FunctionEffect::Alloc;
        if (name == "block") return FunctionEffect::Block;
        if (name == "ffi") return FunctionEffect::Ffi;
        if (name == "throw") return FunctionEffect::Throw;
        if (name == "raise") return FunctionEffect::Raise;
        if (name == "panic") return FunctionEffect::Panic;
        throw std::invalid_argument("unknown Oreslang function effect");
    }
};
} // namespace oreslang
#endif
