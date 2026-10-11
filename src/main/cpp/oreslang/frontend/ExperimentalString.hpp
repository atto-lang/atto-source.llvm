#pragma once
#include <string>
#include <string_view>
namespace oreslang {
// Deliberately opt-in, bounded source-level String vertical slice.
// Linking NativeString.c is required. Not SDK/RPC/TJSV admission.
std::string emit_experimental_string_ir(std::string_view source);
}
