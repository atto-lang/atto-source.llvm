#include "oreslang/types/FunctionEffects.hpp"
#include <cstdlib>
#include <stdexcept>
using namespace oreslang;
// Security and effect checks must run under Release/NDEBUG, not assert().
static void require(bool valid) { if (!valid) std::abort(); }
int main() {
    FunctionEffects declared{};
    declared.bits = static_cast<std::uint32_t>(FunctionEffect::StateRead)
                  | static_cast<std::uint32_t>(FunctionEffect::StateWrite)
                  | static_cast<std::uint32_t>(FunctionEffect::Suspend);
    FunctionEffects read{static_cast<std::uint32_t>(FunctionEffect::StateRead)};
    require(declared.permits(read));
    require(!read.permits(declared));
    require(declared.united({}).permits(declared));
    require(FunctionEffects::parse("state.write") == FunctionEffect::StateWrite);
    require(FunctionEffects::parse("raise") != FunctionEffects::parse("throw"));
    require(FunctionEffects::parse("panic") != FunctionEffects::parse("throw"));
    bool rejected = false;
    try { (void) FunctionEffects::parse("unknown"); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected);
    return 0;
}
