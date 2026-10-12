#include "oreslang/types/FunctionEffects.hpp"
#include <cassert>
#include <stdexcept>
using namespace oreslang;
int main() {
    FunctionEffects declared{};
    declared.bits = static_cast<std::uint32_t>(FunctionEffect::StateRead)
                  | static_cast<std::uint32_t>(FunctionEffect::StateWrite)
                  | static_cast<std::uint32_t>(FunctionEffect::Suspend);
    FunctionEffects read{static_cast<std::uint32_t>(FunctionEffect::StateRead)};
    assert(declared.permits(read));
    assert(!read.permits(declared));
    assert(declared.united({}).permits(declared));
    assert(FunctionEffects::parse("state.write") == FunctionEffect::StateWrite);
    assert(FunctionEffects::parse("raise") != FunctionEffects::parse("throw"));
    assert(FunctionEffects::parse("panic") != FunctionEffects::parse("throw"));
    bool rejected = false;
    try { (void) FunctionEffects::parse("unknown"); }
    catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
    return 0;
}
