#pragma once

#include <filesystem>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace oreslang::project_root_import_detail {
namespace fs = std::filesystem;

// Check the link itself, not only its target. A dangling or swapped symlink
// must not silently fall back from a declared AttoLang module to legacy code.
inline bool exists_marker(const fs::path& path) {
    std::error_code error;
    const auto status = fs::symlink_status(path, error);
    if (error) {
        if (error == std::errc::no_such_file_or_directory) return false;
        throw std::runtime_error("cannot safely inspect native source path");
    }
    return status.type() != fs::file_type::not_found;
}

// Canonical AttoLang source selection. Explicit legacy .ores names remain
// valid during migration; extensionless imports prefer the new .atto file.
// Ambiguous dual-file modules are rejected rather than silently rebinding.
inline fs::path resolve_attolang_source_candidate(fs::path candidate) {
    const auto extension = candidate.extension();
    if (!extension.empty()) {
        if (extension != ".atto" && extension != ".ores")
            throw std::runtime_error(
                "native AttoLang imports require .atto source files "
                "(legacy .ores source files remain accepted)");
        return candidate;
    }
    fs::path modern = candidate;
    modern += ".atto";
    fs::path legacy = std::move(candidate);
    legacy += ".ores";
    const bool has_modern = exists_marker(modern);
    const bool has_legacy = exists_marker(legacy);
    if (has_modern && has_legacy)
        throw std::runtime_error(
            "ambiguous AttoLang module import: both .atto and .ores exist; "
            "choose an explicit filename");
    return has_legacy ? legacy : modern;
}

} // namespace oreslang::project_root_import_detail
