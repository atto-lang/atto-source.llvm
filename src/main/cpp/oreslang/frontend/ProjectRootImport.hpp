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
        if (exists_marker(parent / ".ores.pkg.toml"))
            throw std::runtime_error("native project-root import crosses nested package boundary");
    }
}

struct ManifestLayout {
    // A constrained subset of TOML is admitted for native source identity:
    // a plain quoted [project].root plus either schema_version = "1" or
    // Zed's [package].name. Unsupported / ambiguous authority fails closed.
    fs::path configured_root = ".";
};

inline ManifestLayout validate_manifest(const std::string& manifest_bytes) {
    if (manifest_bytes.size() > 64u * 1024u)
        throw std::runtime_error("native package manifest exceeds 64 KiB");
    // NUL cannot occur in a TOML text manifest. Different consumers must
    // never disagree because one treats a truncated C-string as the file.
    if (manifest_bytes.find('\0') != std::string::npos)
        throw std::runtime_error("native package manifest contains NUL byte");
    if (manifest_bytes.find("\"\"\"") != std::string::npos ||
            manifest_bytes.find("'''") != std::string::npos)
        throw std::runtime_error("native package manifest multiline TOML requires full parser");

    const std::regex version(
        R"ores(^[ \t]*schema_version[ \t]*=[ \t]*"1"[ \t]*(?:#.*)?$)ores");
    const std::regex version_key(
        R"ores(^[ \t]*(?:schema_version|"schema_version"|'schema_version')[ \t]*=.*$)ores");
    const std::regex project_section(
        R"ores(^[ \t]*\[[ \t]*project[ \t]*\][ \t]*(?:#.*)?$)ores");
    const std::regex package_section(
        R"ores(^[ \t]*\[[ \t]*package[ \t]*\][ \t]*(?:#.*)?$)ores");
    const std::regex project_assignment(
        R"ores(^[ \t]*(?:project|"project"|'project')[ \t]*(?:=|\.[ \t]*(?:root|"root"|'root')[ \t]*=).*$)ores");
    const std::regex root_assignment(
        R"ores(^[ \t]*(?:root|"root"|'root')[ \t]*=.*$)ores");
    const std::regex plain_root(
        R"ores(^[ \t]*root[ \t]*=[ \t]*"([A-Za-z0-9_./-]+)"[ \t]*(?:#.*)?$)ores");
    const std::regex name_assignment(
        R"ores(^[ \t]*(?:name|"name"|'name')[ \t]*=.*$)ores");
    const std::regex plain_name(
        R"ores(^[ \t]*name[ \t]*=[ \t]*(['"])([a-z0-9][a-z0-9-]*)\1[ \t]*(?:#.*)?$)ores");

    std::size_t version_count = 0;
    std::size_t name_count = 0;
    std::size_t root_count = 0;
    bool at_top_level = true;
    bool in_project_section = false;
    bool in_package_section = false;
    ManifestLayout layout;
    std::istringstream lines(manifest_bytes);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (std::regex_match(line, project_assignment))
            throw std::runtime_error("native project.root requires full TOML parser");
        if (in_project_section && std::regex_match(line, root_assignment)) {
            std::smatch match;
            if (!std::regex_match(line, match, plain_root) || ++root_count != 1)
                throw std::runtime_error("native project.root must be one plain quoted relative path");
            const std::string root = match[1].str();
            if (root != ".") {
                if (!root.empty() && root.front() == '/')
                    throw std::runtime_error("native project.root cannot be absolute");
                std::size_t begin = 0;
                while (begin <= root.size()) {
                    const auto slash = root.find('/', begin);
                    const auto end = slash == std::string::npos ? root.size() : slash;
                    const auto component = root.substr(begin, end - begin);
                    if (component.empty() || component == "." || component == "..")
                        throw std::runtime_error("native project.root rejects dot/traversal components");
                    if (slash == std::string::npos) break;
                    begin = slash + 1;
                }
            }
            layout.configured_root = fs::path(root);
        }
        if (in_package_section && std::regex_match(line, name_assignment)) {
            std::smatch match;
            if (!std::regex_match(line, match, plain_name) || ++name_count != 1)
                throw std::runtime_error("native Zed [package].name requires a safe single identity");
        }

        const auto first = line.find_first_not_of(" \t");
        if (first != std::string::npos && line[first] == '[') {
            at_top_level = false;
            in_project_section = std::regex_match(line, project_section);
            in_package_section = std::regex_match(line, package_section);
        }
        if (at_top_level && std::regex_match(line, version_key)) {
            if (!std::regex_match(line, version))
                throw std::runtime_error(
                    "native project-root import requires schema_version = \"1\"");
            ++version_count;
        }
    }
    if (version_count != 1 && !(version_count == 0 && name_count == 1))
        throw std::runtime_error(
            "native project-root import requires schema_version = \"1\" or Zed [package].name");
    return layout;
}

/**
 * Java-compatible native source extension policy:
 *   - explicit .atto / .ores stays authoritative;
 *   - implicit imports select an existing .atto entry before legacy .ores;
 *   - a nonregular/dangling/symlinked .atto entry NEVER triggers .ores fallback.
 *
 * This is a lexical leaf-admission check, not a descriptor-pinned security
 * boundary against concurrent filesystem mutation. The loader must keep its
 * source-identity and descriptor checks; separately harden no-follow opens.
 */
inline void require_regular_atto_leaf_if_present(const fs::path& path) {
    std::error_code error;
    const auto status = fs::symlink_status(path, error);
    if (error) {
        if (error == std::errc::no_such_file_or_directory) return;
        throw std::runtime_error("cannot safely inspect canonical .atto source entry");
    }
    if (status.type() == fs::file_type::not_found) return;
    if (status.type() != fs::file_type::regular)
        throw std::runtime_error(
            "canonical .atto source entry must be a regular non-symlink file");
}

inline fs::path resolve_native_source_suffix(fs::path candidate) {
    auto extension = candidate.extension().string();
    for (char& value : extension)
        value = static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
    if (!extension.empty()) {
        if (extension != ".atto" && extension != ".ores")
            throw std::runtime_error(
                "native module source must use .atto or legacy .ores extension");
        if (extension == ".atto") require_regular_atto_leaf_if_present(candidate);
        return candidate;
    }
    fs::path modern = candidate;
    modern += ".atto";
    fs::path legacy = candidate;
    legacy += ".ores";
    if (exists_marker(modern)) {
        require_regular_atto_leaf_if_present(modern);
        return modern;
    }
    if (exists_marker(legacy)) return legacy;
    // Let canonical admission report the missing source. Never search other
    // file types, and never downgrade an existing (even invalid) .atto entry.
    return modern;
}

inline void enforce_relative_import_package_boundary(
        const fs::path& importing_source, const fs::path& dependency,
        bool require_package_manifest = false) {
    std::error_code error;
    const auto source = fs::canonical(importing_source, error);
    if (error)
        throw std::runtime_error("cannot resolve native importing source");

    fs::path cursor = source.parent_path();
    fs::path package_root;
    for (;;) {
        const fs::path manifest = cursor / ".ores.pkg.toml";
        if (exists_marker(manifest)) {
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
    // Legacy ./ and ../ imports may remain package-less. Expanded absolute
    // environment imports, however, require a real package boundary so an
    // untrusted process variable cannot redirect the compiler anywhere.
    if (package_root.empty()) {
        if (require_package_manifest)
            throw std::runtime_error("absolute import requires nearest .ores.pkg.toml");
        return;
    }

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
    fs::path configured_root = ".";
    for (;;) {
        const fs::path manifest = cursor / ".ores.pkg.toml";
        if (exists_marker(manifest)) {
            // Never canonicalize away a mutable manifest symlink, even if it
            // currently points to a regular file in the same directory.
            // The marker is a package authority, not a relocatable alias.
            std::error_code regular_error;
            const auto marker_status = fs::symlink_status(manifest, regular_error);
            if (regular_error || marker_status.type() != fs::file_type::regular)
                throw std::runtime_error(
                    "native package manifest must be a regular non-symlink file");
            const auto physical_manifest = fs::canonical(manifest, error);
            if (error)
                throw std::runtime_error("cannot resolve native package manifest");
            if (physical_manifest.parent_path() != cursor)
                throw std::runtime_error(
                    "native package manifest must reside within package directory");
            // Reopen the already-canonical path so a manifest symlink swap after
            // the locality check cannot redirect the bytes we validate.
            const auto layout = validate_manifest(read_manifest(physical_manifest));
            configured_root = layout.configured_root;
            package_root = cursor;
            break;
        }
        if (cursor == cursor.root_path()) break;
        const auto parent = cursor.parent_path();
        if (parent.empty() || parent == cursor) break;
        cursor = parent;
    }
    if (package_root.empty())
        throw std::runtime_error("native +/ imports require nearest .ores.pkg.toml");

    const auto suffix = raw.substr(2);
    validate_suffix(suffix);
    // Never spell the default root as "pkg/.": the parent walk in
    // reject_nested_package would treat "pkg/." as a nested directory.
    const fs::path source_root_spelled = configured_root == fs::path(".")
        ? package_root : package_root / configured_root;
    const fs::path source_root = fs::canonical(source_root_spelled, error);
    if (error)
        throw std::runtime_error("cannot resolve configured native project.root");
    const fs::path physical_root = fs::canonical(package_root, error);
    if (error)
        throw std::runtime_error("cannot resolve native package root");
    if (source_root != physical_root &&
            !strict_descendant(physical_root, source_root))
        throw std::runtime_error("configured native project.root escapes package root");

    fs::path candidate = source_root_spelled / fs::path(std::string(suffix));
    candidate = resolve_native_source_suffix(candidate);
    // Compare like with like: canonical configured root against the canonical
    // package root, and the spelled candidate against the spelled package root
    // (temp dirs and checkouts may sit behind symlinks such as /var -> /private/var).
    if (source_root != physical_root)
        reject_nested_package(physical_root, source_root);
    reject_nested_package(package_root, candidate);

    const auto target = fs::canonical(candidate, error);
    if (error)
        throw std::runtime_error("cannot resolve native project-root import");
    reject_nested_package(physical_root, target);
    if (!strict_descendant(source_root, target))
        throw std::runtime_error("native project-root import escapes configured project.root");
    // A canonical path may still identify a directory or other non-file.
    // Never hand it to the native source loader as an executable import.
    std::error_code leaf_error;
    if (!fs::is_regular_file(target, leaf_error) || leaf_error)
        throw std::runtime_error("native project-root import source must be a regular file");
    return target;
}

} // namespace oreslang
