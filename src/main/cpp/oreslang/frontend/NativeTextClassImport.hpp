#pragma once
#include <filesystem>
#include <string>
#include <string_view>

namespace oreslang {
// Isolated, fail-closed host-native source slice for imported, stateless classes
// with string-returning methods. Does not pretend to lower arbitrary Oreslang.
bool is_native_text_class_import(std::string_view source);
std::string emit_native_text_class_import(const std::filesystem::path& entry,
                                          std::string_view source);
}
