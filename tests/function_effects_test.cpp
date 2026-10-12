#include "oreslang/types/FunctionEffects.hpp"
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>

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
        bool rejected = false;
        try {
            (void) FunctionEffects::parse("unknown");
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        require(rejected, "unknown effects must be rejected");
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << "function effects conformance: " << failure.what() << '\n';
        return 1;
    }
}
