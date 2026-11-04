#pragma once

#include "oreslang/frontend/SourceInput.hpp"
#include "oreslang/frontend/ProjectRootImport.hpp"
#include <filesystem>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace oreslang {
namespace fs = std::filesystem;

// Trusted, per-linked-build host path -> logical unit ID bridge. It does NOT
// grant compile-time macro authority to external roots, and is not a source
// reader: SourceInput binds opened descriptors to the caller's SourceIdentity.
class HostLogicalSourceUnitIds final {
    struct Root {
        fs::path lexical;
        fs::path real;
        std::string prefix;
    };
    struct Admitted {
        fs::path physical;
        SourceIdentity identity;
    };
    std::vector<Root> roots_;
    std::unordered_map<std::string, Admitted> by_id_;

    static fs::path absolute(const fs::path& path) {
        if (path.empty()) throw std::runtime_error("native source path is empty");
        return fs::absolute(path).lexically_normal();
    }
    static bool beneath(const fs::path& root, const fs::path& path,
                        fs::path& relative) {
        relative = path.lexically_relative(root);
        if (relative.empty() || relative == ".") return false;
        for (const auto& part : relative) {
            if (part == "..") return false;
        }
        return !relative.is_absolute();
    }
    static void checked_ascii(std::string_view name) {
        if (name.empty() || name.size() > 4096)
            throw std::runtime_error("native logical source-unit ID exceeds canonical budget");
        // Conservative M0: reject all non-ASCII until a shared Unicode
        // General_Category implementation is reviewed. Java admits additional
        // safe Unicode names; native must not pretend equivalent acceptance.
        for (unsigned char ch : name) {
            if (ch < 0x21 || ch > 0x7e || ch == ':' || ch == '\\' ||
                ch == '"' || ch == '<' || ch == '>' || ch == '|' ||
                ch == '?' || ch == '*')
                throw std::runtime_error("native logical source-unit ID has noncanonical characters");
        }
    }
    static void require_canonical_id(std::string_view name) {
        checked_ascii(name);
        if (name.front() == '/' || name.find("//") != name.npos ||
            name.find("/./") != name.npos || name.find("/../") != name.npos ||
            name.ends_with("/.") || name.ends_with("/..") ||
            name.starts_with("./") || name.starts_with("../") ||
            name.back() == '/')
            throw std::runtime_error("native logical source-unit ID must be root-relative");
        if (!name.starts_with("project/") && !name.starts_with("external/"))
            throw std::runtime_error("native source-unit ID lacks a declared root namespace");
        if (name.starts_with("external/")) {
            const auto slash = name.find('/', 9);
            if (slash == name.npos || slash == 9)
                throw std::runtime_error("native external source-unit ID lacks root ordinal");
            for (std::size_t i = 9; i < slash; ++i)
                if (name[i] < '0' || name[i] > '9')
                    throw std::runtime_error("native external root ordinal is invalid");
        }
    }
    static void validate_root_marker(const fs::path& marker, bool canonical) {
        const auto path = marker.string();
        const auto expected = capture_source_identity(
            path.c_str(), 64u * 1024u, "native project marker exceeds 64 KiB");
        const auto contents = read_regular_source(
            path.c_str(), 64u * 1024u,
            "native project marker exceeds 64 KiB", nullptr, &expected);
        require_valid_source_utf8(contents);
        if (!canonical) {
            project_root_import_detail::validate_manifest(contents);
            return;
        }
        // Safe canonical M0 TOML subset; refuse unknown metadata rather than
        // accepting it as an implicit root authority. Full TOML is pending.
        const std::regex root_line(R"(^[ \t]*root[ \t]*=[ \t]*"\."[ \t]*(?:#.*)?$)");
        std::istringstream input(contents);
        std::string line;
        bool project = false;
        bool root = false;
        while (std::getline(input, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const auto first = line.find_first_not_of(" \t");
            if (first == std::string::npos || line[first] == '#') continue;
            if (line == "[project]" && !project) {
                project = true;
                continue;
            }
            if (project && !root && std::regex_match(line, root_line)) {
                root = true;
                continue;
            }
            throw std::runtime_error(
                "unsupported canonical .atto.pkg.toml metadata or duplicate root");
        }
        if (!project || !root)
            throw std::runtime_error(
                "canonical .atto.pkg.toml requires [project] root = \".\"");
    }
    static fs::path discover_project_root(const fs::path& entry) {
        const auto path = absolute(entry);
        fs::path cursor = path.parent_path();
        for (;;) {
            for (const char* marker : {".atto.pkg.toml", ".ores.pkg.toml"}) {
                const fs::path candidate = cursor / marker;
                std::error_code error;
                const auto status = fs::symlink_status(candidate, error);
                if (error == std::errc::no_such_file_or_directory) continue;
                if (error) throw std::runtime_error("cannot inspect source project root marker");
                if (status.type() != fs::file_type::not_found) {
                    if (!fs::is_regular_file(status))
                        throw std::runtime_error("source project root marker must be regular and not symlinked");
                    validate_root_marker(candidate, std::string_view(marker) == ".atto.pkg.toml");
                    return cursor;
                }
            }
            if (cursor == cursor.root_path()) break;
            const auto parent = cursor.parent_path();
            if (parent.empty() || parent == cursor) break;
            cursor = parent;
        }
        // Package-less entry: do not invent ambient import-root authority.
        return path.parent_path();
    }
    void add_root(const fs::path& directory, std::string prefix) {
        const auto lexical = absolute(directory);
        std::error_code error;
        const auto real = fs::canonical(lexical, error);
        if (error || !fs::is_directory(real))
            throw std::runtime_error("declared native source root is unavailable");
        roots_.push_back(Root{lexical, real, std::move(prefix)});
    }
public:
    explicit HostLogicalSourceUnitIds(
        const fs::path& entry,
        const std::vector<fs::path>& explicitly_owned_external_roots = {}) {
        add_root(discover_project_root(entry), "project");
        for (std::size_t i = 0; i < explicitly_owned_external_roots.size(); ++i)
            add_root(explicitly_owned_external_roots[i], "external/" + std::to_string(i + 1));
    }

    // Reject arbitrary caller IDs even if they happen to name an existing unit.
    static std::string validated_logical_id(const std::string& logical) {
        require_canonical_id(logical);
        return logical;
    }

    std::string admit(const fs::path& source_path, const SourceIdentity& opened) {
        if (!opened.valid)
            throw std::runtime_error("native source identity requires a verified opened descriptor");
        const auto lexical = absolute(source_path);
        std::error_code error;
        const auto physical = fs::canonical(lexical, error);
        if (error) throw std::runtime_error("cannot canonicalize native source path");
        const auto status = fs::status(physical, error);
        if (error || !fs::is_regular_file(status))
            throw std::runtime_error("native source identity requires a regular file");
        const auto same_path_identity = capture_source_identity(
            physical.string().c_str(), 8u * 1024u * 1024u,
            "native source exceeds 8 MiB identity budget");
        if (!ores_source_identity_equal(same_path_identity, opened))
            throw std::runtime_error("native source identity changed after descriptor read");
        const auto links = fs::hard_link_count(physical, error);
        if (error || links != 1)
            throw std::runtime_error("native source identity rejects hardlink aliases");

        std::string logical;
        for (const auto& root : roots_) {
            fs::path relative;
            if (!beneath(root.lexical, lexical, relative)) continue;
            // Root-directory symlinks may be configured, but no component
            // BELOW a declared root may redirect or collapse source identity.
            if ((root.real / relative).lexically_normal() != physical)
                throw std::runtime_error("native source identity rejects symlink/alias path");
            logical = root.prefix + "/" + relative.generic_string();
            break;
        }
        if (logical.empty())
            throw std::runtime_error("native source is outside declared source roots");
        require_canonical_id(logical);
        const auto known = by_id_.find(logical);
        if (known != by_id_.end()) {
            if (known->second.physical != physical ||
                !ores_source_identity_equal(known->second.identity, opened))
                throw std::runtime_error("native logical source-unit ID changed its physical owner");
            return logical;
        }
        for (const auto& [other, record] : by_id_) {
            if (fs::equivalent(record.physical, physical, error) && !error)
                throw std::runtime_error("native source file has duplicate logical unit IDs");
            if (error) throw std::runtime_error("cannot compare native source file identities");
        }
        by_id_.emplace(logical, Admitted{physical, opened});
        return logical;
    }
};
} // namespace oreslang
