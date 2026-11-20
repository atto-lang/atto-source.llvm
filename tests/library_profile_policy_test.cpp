#include "oreslang/frontend/LibraryProfile.hpp"

#include <cassert>
#include <string_view>

int main() {
    using oreslang::frontend::LibraryProfile;
    using oreslang::frontend::default_library_profile;
    using oreslang::frontend::library_profile_admission_error;
    using oreslang::frontend::parse_library_profile;

    assert(parse_library_profile("core") == LibraryProfile::Core);
    assert(parse_library_profile("std") == LibraryProfile::Std);

    for (std::string_view invalid : {
             "alloc", "actor", "otp", "hosted", "freestanding-core", "CORE", ""}) {
        assert(!parse_library_profile(invalid).has_value());
    }

    assert(default_library_profile(false) == LibraryProfile::Std);
    assert(default_library_profile(true) == LibraryProfile::Core);

    assert(library_profile_admission_error(LibraryProfile::Std, false).empty());
    assert(library_profile_admission_error(LibraryProfile::Core, true).empty());

    const auto std_on_core =
        library_profile_admission_error(LibraryProfile::Std, true);
    assert(std_on_core.find("ores.std") != std::string_view::npos);

    const auto core_on_host =
        library_profile_admission_error(LibraryProfile::Core, false);
    assert(core_on_host.find("ores.core") != std::string_view::npos);
    assert(core_on_host.find("freestanding") != std::string_view::npos);

    return 0;
}
