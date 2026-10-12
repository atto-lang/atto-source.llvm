#include "oreslang/types/FunctionEffects.hpp"
#include <stdexcept>
#include <string>
#include <string_view>

using namespace oreslang;

static void require(bool condition, std::string_view message) {
    // CTest runs Release builds too. Standard assert() disappears under
    // NDEBUG, accidentally converting important effect checks to no-ops.
    if (!condition) throw std::runtime_error(std::string(message));
}

int main() {
    FunctionEffects declared{};
    declared.bits = static_cast<std::uint32_t>(FunctionEffect::StateRead)
                  | static_cast<std::uint32_t>(FunctionEffect::StateWrite)
                  | static_cast<std::uint32_t>(FunctionEffect::Suspend);
    const FunctionEffects read{static_cast<std::uint32_t>(FunctionEffect::StateRead)};
    const FunctionEffects write{static_cast<std::uint32_t>(FunctionEffect::StateWrite)};
    const FunctionEffects none{};

    require(declared.permits(read), "declared effects must permit a read");
    require(declared.permits(write), "declared effects must permit a write");
    require(!read.permits(declared), "read-only effect must not permit write/suspend");
    require(none.permits(none), "empty effect set must permit empty");
    require(!none.permits(read), "empty effect set cannot authorize a read");
    require(read.united(write).permits(write), "effect union must contain write");
    require(declared.united(none).permits(declared), "union with empty must be identity");
    require(declared.has(FunctionEffect::Suspend), "suspend effect must be present");
    require(!read.has(FunctionEffect::Panic), "read-only effect must not imply panic");

    require(FunctionEffects::parse("state.read") == FunctionEffect::StateRead,
            "state.read parsing");
    require(FunctionEffects::parse("state.write") == FunctionEffect::StateWrite,
            "state.write parsing");
    require(FunctionEffects::parse("raise") != FunctionEffects::parse("throw"),
            "raise/throw effects must remain distinct");
    require(FunctionEffects::parse("panic") != FunctionEffects::parse("throw"),
            "panic/throw effects must remain distinct");

    for (const std::string_view invalid : {"unknown", "", "StateWrite"}) {
        bool rejected = false;
        try { (void) FunctionEffects::parse(invalid); }
        catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "unknown or incorrectly cased effect must fail closed");
    }
    return 0;
}
