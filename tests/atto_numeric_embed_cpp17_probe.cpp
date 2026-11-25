#include <string_view>

// This is a C++ embedding syntax probe, NOT compiled AttoLang guest evidence.
static constexpr std::string_view kNumericAttoSource =
#include "oreslang/frontend/GeneratedNumericAtto.inc"
;
static_assert(kNumericAttoSource.size() == 657, "canonical source size changed");
static_assert(kNumericAttoSource.find("pub fnc min_int") != std::string_view::npos,
              "generated numeric source is incomplete");
static_assert(kNumericAttoSource.find("// Compiler-owned ores.core source.") == 0,
              "first source line must be preserved");
static_assert(kNumericAttoSource.find("\nend\n") != std::string_view::npos,
              "last source line must be preserved");
int main() { return 0; }
