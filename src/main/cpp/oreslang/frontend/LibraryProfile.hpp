#pragma once

#include <optional>
#include <string_view>

namespace oreslang::frontend {

enum class LibraryProfile {
    Core,
    Std
};

inline std::optional<LibraryProfile> parse_library_profile(std::string_view value) noexcept {
    if (value == "core") return LibraryProfile::Core;
    if (value == "std") return LibraryProfile::Std;
    return std::nullopt;
}

inline LibraryProfile default_library_profile(bool freestanding_target) noexcept {
    return freestanding_target ? LibraryProfile::Core : LibraryProfile::Std;
}

inline std::string_view library_profile_admission_error(
        LibraryProfile profile,
        bool freestanding_target) noexcept {
    if (profile == LibraryProfile::Std && freestanding_target) {
        return "ores.std requires a certified hosted target; freestanding targets use ores.core";
    }
    if (profile == LibraryProfile::Core && !freestanding_target) {
        return "ores.core native execution currently requires a certified freestanding target";
    }
    return {};
}

} // namespace oreslang::frontend
