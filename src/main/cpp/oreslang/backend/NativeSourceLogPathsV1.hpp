#pragma once
// Pure C++17, cold-path source-log provenance admission. No LLVM SDK required.
// Prevent two source-ID identities for the same symlinked filename or alias.
// This proves local physical path binding, not source review authorization.
#include <filesystem>
#include <stdexcept>

namespace oreslang::telemetry {

inline void require_exact_source_log_paths(
        const std::filesystem::path& package_root,
        const std::filesystem::path& source_unit,
        const std::filesystem::path& manifest_unit) {
    namespace fs = std::filesystem;
    // std::filesystem::filesystem_error embeds absolute host paths in what().
    // A malicious source/manifest argument must not disclose private workspace
    // roots through the compiler diagnostic stream. Keep this function's public
    // failure surface independent of filesystem paths.
    try {
        const fs::path root = fs::canonical(package_root);
        const auto checked = [&](const fs::path& supplied) {
            if (supplied.empty() || supplied.is_absolute() ||
                !supplied.has_filename() ||
                supplied.lexically_normal() != supplied ||
                supplied == "." ||
                fs::canonical(root / supplied) != root / supplied ||
                !fs::is_regular_file(root / supplied)) {
                throw std::runtime_error(
                    "source-log requires canonical package-relative regular files");
            }
        };
        checked(source_unit);
        checked(manifest_unit);
        if (fs::equivalent(root / source_unit, root / manifest_unit)) {
            throw std::runtime_error("source-log manifest must not alias guest source");
        }
    } catch (const fs::filesystem_error&) {
        throw std::runtime_error(
            "source-log file cannot be safely resolved as a canonical regular file");
    }
}

} // namespace oreslang::telemetry
