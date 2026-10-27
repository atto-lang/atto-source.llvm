#pragma once

#include <optional>
#include <string_view>

namespace oreslang::frontend {

struct CoreLibrarySource {
    std::string_view canonical_import;
    std::string_view module_name;
    std::string_view source;
};

inline std::optional<CoreLibrarySource> core_library_source(
        std::string_view import_path) noexcept {
    if (import_path == "ores.core/numeric") {
        return CoreLibrarySource{
            "ores.core/numeric",
            "numeric",
            R"ORES(// Compiler-owned ores.core source.
define module numeric as
  pub fnc min_int(int left, int right): int {
    return left < right ? left : right;
  }
  pub fnc max_int(int left, int right): int {
    return left > right ? left : right;
  }
  pub fnc clamp_int(int value, int lower, int upper): int {
    return value < lower ? lower : (value > upper ? upper : value);
  }
end
)ORES"
        };
    }
    return std::nullopt;
}

inline bool is_reserved_core_import(std::string_view import_path) noexcept {
    return import_path == "ores.core"
            || import_path.rfind("ores.core/", 0) == 0;
}

inline bool is_reserved_std_import(std::string_view import_path) noexcept {
    return import_path == "ores.std"
            || import_path.rfind("ores.std/", 0) == 0
            || import_path == "std"
            || import_path.rfind("std/", 0) == 0;
}

} // namespace oreslang::frontend
