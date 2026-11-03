#pragma once

#include <cctype>
#include <filesystem>
#include <functional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace oreslang {

namespace project_root_import_detail {
namespace fs = std::filesystem;

inline bool exists_marker(const fs::path& path) {
    std::error_code error;
    const auto status = fs::symlink_status(path, error);
    if (error) {
        if (error == std::errc::no_such_file_or_directory) return false;
        throw std::runtime_error("cannot safely inspect native package boundary");
    }
    return status.type() != fs::file_type::not_found;
}

inline fs::path find_package_manifest(const fs::path& directory) {
    const fs::path modern = directory / ".atto.pkg.toml";
    const fs::path legacy = directory / ".ores.pkg.toml";
    const bool has_modern = exists_marker(modern);
    const bool has_legacy = exists_marker(legacy);
    if (has_modern && has_legacy)
        throw std::runtime_error(
            "native package directory has conflicting .atto and .ores manifests");
    return has_modern ? modern : has_legacy ? legacy : fs::path{};
}

inline bool strict_descendant(const fs::path& root, const fs::path& path) {
    auto root_it = root.begin();
    auto path_it = path.begin();
    for (; root_it != root.end(); ++root_it, ++path_it) {
        if (path_it == path.end() || *root_it != *path_it) return false;
    }
    return path_it != path.end();
}

inline void reject_nested_package(const fs::path& root, const fs::path& path) {
    if (!strict_descendant(root, path))
        throw std::runtime_error("native project-root import escapes physical package root");
    for (fs::path parent = path.parent_path(); parent != root; parent = parent.parent_path()) {
        if (parent.empty() || parent == parent.root_path())
            throw std::runtime_error("native project-root import escapes physical package root");
        if (!find_package_manifest(parent).empty())
            throw std::runtime_error("native project-root import crosses nested package boundary");
    }
}

inline void validate_manifest(const std::string& manifest_bytes) {
    if (manifest_bytes.find("\"\"\"") != std::string::npos ||
            manifest_bytes.find("'''") != std::string::npos)
        throw std::runtime_error("native package manifest multiline TOML requires full parser");

    const std::regex version(
        R"ores(^[ \t]*schema_version[ \t]*=[ \t]*"1"[ \t]*(?:#.*)?$)ores");
    const std::regex version_key(
        R"ores(^[ \t]*(?:schema_version|"schema_version"|'schema_version')[ \t]*=.*$)ores");
    const std::regex project_section(
        R"ores(^[ \t]*\[[ \t]*(?:project|"project"|'project')[ \t]*\][ \t]*(?:#.*)?$)ores");
    const std::regex project_assignment(
        R"ores(^[ \t]*(?:project|"project"|'project')[ \t]*(?:=|\.[ \t]*(?:root|"root"|'root')[ \t]*=).*$)ores");
    const std::regex root_assignment(
        R"ores(^[ \t]*(?:root|"root"|'root')[ \t]*=.*$)ores");

    std::size_t version_count = 0;
    bool in_root_section = true;
    bool in_project_section = false;
    std::istringstream lines(manifest_bytes);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (std::regex_match(line, project_assignment) ||
                (in_project_section && std::regex_match(line, root_assignment)))
            throw std::runtime_error("native project.root requires full TOML parser");

        const auto first = line.find_first_not_of(" \t");
        if (first != std::string::npos && line[first] == '[') {
            in_root_section = false;
            in_project_section = std::regex_match(line, project_section);
        }
        if (in_root_section && std::regex_match(line, version_key)) {
            if (!std::regex_match(line, version))
                throw std::runtime_error(
                    "native project-root import requires schema_version = \"1\"");
            ++version_count;
        }
    }
    if (version_count != 1)
        throw std::runtime_error(
            "native project-root import requires schema_version = \"1\"");
}

/**
 * Resolve only the two admitted native source extensions. Extensionless
 * imports are intentionally ambiguous if both canonical .atto
 * and legacy .ores names exist. Migration must never silently select one twin.
 * Inspect directory entries (not only target existence), so dangling symlinks
 * and same-inode aliases still count as conflicting source names.
 */
inline fs::path resolve_native_source_suffix(fs::path candidate) {
    auto extension = candidate.extension().string();
    for (char& value : extension)
        value = static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
    if (!extension.empty()) {
        if (extension != ".atto" && extension != ".ores")
            throw std::runtime_error(
                "native module source must use .atto or legacy .ores extension");
        return candidate;
    }
    fs::path modern = candidate;
    modern += ".atto";
    fs::path legacy = candidate;
    legacy += ".ores";
    const bool has_modern = exists_marker(modern);
    const bool has_legacy = exists_marker(legacy);
    if (has_modern && has_legacy)
        throw std::runtime_error(
            "ambiguous native source import: both .atto and .ores exist; use explicit extension");
    if (has_modern) return modern;
    if (has_legacy) return legacy;
    // Allow the caller's canonical-resolution path to produce its normal
    // missing-file diagnostic. Never search unrelated extension types.
    return modern;
}

inline void enforce_relative_import_package_boundary(
        const fs::path& importing_source, const fs::path& dependency) {
    std::error_code error;
    const auto source = fs::canonical(importing_source, error);
    if (error)
        throw std::runtime_error("cannot resolve native importing source");

    fs::path cursor = source.parent_path();
    fs::path package_root;
    for (;;) {
        const fs::path manifest = find_package_manifest(cursor);
        if (!manifest.empty()) {
            std::error_code regular_error;
            if (!fs::is_regular_file(manifest, regular_error) || regular_error)
                throw std::runtime_error("native package manifest must be a regular file");
            const auto physical_manifest = fs::canonical(manifest, error);
            if (error)
                throw std::runtime_error("cannot resolve native package manifest");
            if (physical_manifest.parent_path() != cursor)
                throw std::runtime_error(
                    "native package manifest must reside within package directory");
            package_root = cursor;
            break;
        }
        if (cursor == cursor.root_path()) break;
        const auto parent = cursor.parent_path();
        if (parent.empty() || parent == cursor) break;
        cursor = parent;
    }
    // Preserve package-less legacy relative imports. Once a package manifest
    // exists, however, relative ../ must not become a package-boundary bypass.
    if (package_root.empty()) return;

    const auto physical_root = fs::canonical(package_root, error);
    if (error)
        throw std::runtime_error("cannot resolve native package root");
    const auto physical_dependency = fs::canonical(dependency, error);
    if (error)
        throw std::runtime_error("cannot resolve native import dependency");
    reject_nested_package(physical_root, physical_dependency);
}

inline void validate_suffix(std::string_view suffix) {
    if (suffix.empty())
        throw std::runtime_error("native project-root import requires a non-empty path");
    std::size_t start = 0;
    while (start <= suffix.size()) {
        const auto slash = suffix.find('/', start);
        const auto end = slash == std::string_view::npos ? suffix.size() : slash;
        const auto segment = suffix.substr(start, end - start);
        if (segment.empty() || segment == "." || segment == "..")
            throw std::runtime_error(
                "native project-root import rejects empty/dot/traversal components");
        for (unsigned char ch : segment) {
            const bool ascii_alnum =
                (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                (ch >= '0' && ch <= '9');
            if (!(ascii_alnum || ch == '_' || ch == '-' || ch == '.'))
                throw std::runtime_error(
                    "native project-root import contains a noncanonical path component");
        }
        if (slash == std::string_view::npos) break;
        start = slash + 1;
    }
}

} // namespace project_root_import_detail

inline std::filesystem::path resolve_project_root_import(
        const std::filesystem::path& importing_source,
        std::string_view raw,
        const std::function<std::string(const std::filesystem::path&)>& read_manifest) {
    namespace fs = std::filesystem;
    using namespace project_root_import_detail;

    if (raw.rfind("+/", 0) != 0)
        throw std::runtime_error("native project-root resolver requires +/ import");

    std::error_code error;
    const auto source = fs::canonical(importing_source, error);
    if (error)
        throw std::runtime_error("cannot resolve native importing source");

    fs::path cursor = source.parent_path();
    fs::path package_root;
    for (;;) {
        const fs::path manifest = find_package_manifest(cursor);
        if (!manifest.empty()) {
            std::error_code regular_error;
            if (!fs::is_regular_file(manifest, regular_error) || regular_error)
                throw std::runtime_error("native package manifest must be a regular file");
            const auto physical_manifest = fs::canonical(manifest, error);
            if (error)
                throw std::runtime_error("cannot resolve native package manifest");
            if (physical_manifest.parent_path() != cursor)
                throw std::runtime_error(
                    "native package manifest must reside within package directory");
            // Reopen the already-canonical path so a manifest symlink swap after
            // the locality check cannot redirect the bytes we validate.
            validate_manifest(read_manifest(physical_manifest));
            package_root = cursor;
            break;
        }
        if (cursor == cursor.root_path()) break;
        const auto parent = cursor.parent_path();
        if (parent.empty() || parent == cursor) break;
        cursor = parent;
    }
    if (package_root.empty())
        throw std::runtime_error("native +/ imports require nearest .ores.pkg.toml or .atto.pkg.toml");

    const auto suffix = raw.substr(2);
    validate_suffix(suffix);
    fs::path candidate = package_root / fs::path(std::string(suffix));
    candidate = resolve_native_source_suffix(candidate);

    const auto physical_root = fs::canonical(package_root, error);
    if (error)
        throw std::runtime_error("cannot resolve native package root");

    reject_nested_package(package_root, candidate);

    const auto target = fs::canonical(candidate, error);
    if (error)
        throw std::runtime_error("cannot resolve native project-root import");
    reject_nested_package(physical_root, target);
    return target;
}

} // namespace oreslang
