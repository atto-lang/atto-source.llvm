#pragma once

#include "oreslang/frontend/SourceInput.hpp"
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace oreslang {
namespace frontend {
namespace logical_source_detail {
namespace fs = std::filesystem;

inline bool marker_present(const fs::path& candidate) {
    std::error_code error;
    const auto status = fs::symlink_status(candidate, error);
    if (error == std::errc::no_such_file_or_directory) return false;
    if (error) throw std::runtime_error("cannot inspect source package ownership");
    return status.type() != fs::file_type::not_found;
}

// Same search as root_for_entry, but returns the *spelled* directory. Lexical
// ownership checks must compare like with like: a spelled candidate such as
// /var/... (macOS: /var -> /private/var) is never lexically under a canonical root.
inline fs::path spelled_root_for_entry(const fs::path& entry) {
    const auto absolute = fs::absolute(entry).lexically_normal();
    for (auto dir = absolute.parent_path(); !dir.empty();) {
        if (marker_present(dir / ".atto.pkg.toml") || marker_present(dir / ".ores.pkg.toml"))
            return dir;
        const auto parent = dir.parent_path();
        if (parent.empty() || parent == dir) break;
        dir = parent;
    }
    return absolute.parent_path();
}

inline bool lexically_under(const fs::path& absolute, const fs::path& root) {
    const auto relative = absolute.lexically_relative(root);
    return !relative.empty() && relative != fs::path(".") &&
        !relative.is_absolute() && *relative.begin() != fs::path("..");
}

inline fs::path root_for_entry(const fs::path& entry) {
    const auto absolute = fs::absolute(entry).lexically_normal();
    for (auto dir = absolute.parent_path(); !dir.empty();) {
        const auto canonical = dir / ".atto.pkg.toml";
        const auto legacy = dir / ".ores.pkg.toml";
        if (marker_present(canonical) || marker_present(legacy)) {
            for (const auto& manifest : {canonical, legacy}) {
                if (!marker_present(manifest)) continue;
                std::error_code error;
                const auto status = fs::symlink_status(manifest, error);
                if (error || !fs::is_regular_file(status))
                    throw std::runtime_error("source package manifest must be a regular non-symlink file");
            }
            return fs::canonical(dir);
        }
        const auto parent = dir.parent_path();
        if (parent.empty() || parent == dir) break;
        dir = parent;
    }
    // Package-less legacy entrypoints are owned by their entry directory only.
    // Do not infer authority from an ambient filesystem ancestor or PATH.
    return fs::canonical(absolute.parent_path());
}

inline bool under(const fs::path& root, const fs::path& source) {
    auto a = root.begin(), b = source.begin();
    for (; a != root.end(); ++a, ++b)
        if (b == source.end() || *a != *b) return false;
    return b != source.end();
}

inline bool forbidden_codepoint(std::uint32_t cp) {
    if (cp == ':' || cp <= 0x1f || (cp >= 0x7f && cp <= 0x9f)) return true;
    if (cp == 0x00ad || cp == 0x061c || cp == 0x06dd || cp == 0x070f ||
        cp == 0x08e2 || cp == 0x180e || cp == 0xfeff || cp == 0x110bd ||
        cp == 0x110cd || cp == 0xe0001) return true;
    if ((cp >= 0x0600 && cp <= 0x0605) ||
        (cp >= 0x0890 && cp <= 0x0891) ||
        (cp >= 0x200b && cp <= 0x200f) ||
        (cp >= 0x2028 && cp <= 0x202e) ||
        (cp >= 0x2060 && cp <= 0x206f) ||
        (cp >= 0xfff9 && cp <= 0xfffb) ||
        (cp >= 0x13430 && cp <= 0x1345f) ||
        (cp >= 0x1bca0 && cp <= 0x1bca3) ||
        (cp >= 0x1d173 && cp <= 0x1d17a) ||
        (cp >= 0xe0020 && cp <= 0xe007f) ||
        (cp >= 0xfdd0 && cp <= 0xfdef) ||
        (cp & 0xfffeu) == 0xfffeu) return true;
    return false;
}

// Deliberately validate the original spelling before path normalization;
// Unicode bidi/control characters must not disappear through a dot segment.
inline void validate_logical_id(std::string_view value) {
    if (value.empty() || value.size() > 4096 || value.front() == '/' ||
        value.find('\\') != std::string_view::npos)
        throw std::runtime_error("native logical source ID is not bounded relative UTF-8");
    require_valid_source_utf8(value);
    for (std::size_t i = 0; i < value.size();) {
        const auto ch = static_cast<unsigned char>(value[i]);
        std::uint32_t cp = ch;
        std::size_t length = 1;
        if (ch >= 0xf0) { cp = ch & 0x07u; length = 4; }
        else if (ch >= 0xe0) { cp = ch & 0x0fu; length = 3; }
        else if (ch >= 0xc0) { cp = ch & 0x1fu; length = 2; }
        for (std::size_t j = 1; j < length; ++j)
            cp = (cp << 6u) | (static_cast<unsigned char>(value[i+j]) & 0x3fu);
        if (forbidden_codepoint(cp))
            throw std::runtime_error("native logical source ID contains nonportable Unicode or colon");
        i += length;
    }
    std::size_t start = 0;
    while (start < value.size()) {
        const auto slash = value.find('/', start);
        const auto end = slash == std::string_view::npos ? value.size() : slash;
        const auto segment = value.substr(start, end - start);
        if (segment.empty() || segment == "." || segment == "..")
            throw std::runtime_error("native logical source ID contains traversal or empty segment");
        start = end + 1;
        if (slash == std::string_view::npos) break;
        if (start == value.size())
            throw std::runtime_error("native logical source ID contains trailing slash");
    }
}

// Pure lexical ownership check: call this BEFORE suffix discovery or any
// status/canonical/read operation on an untrusted import path. In particular,
// existence of a file outside the root must not influence diagnostics.
inline fs::path checked_relative_source_path(const fs::path& original,
                                             const fs::path& root) {
    const auto absolute = fs::absolute(original).lexically_normal();
    const auto relative = absolute.lexically_relative(root);
    if (relative.empty() || relative == fs::path(".") ||
        relative.is_absolute() || *relative.begin() == fs::path(".."))
        throw std::runtime_error("native source is outside the admitted project root");
    return relative;
}

inline void no_symlink_components(const fs::path& original, const fs::path& root_in) {
    // A canonical root (e.g. /private/var/...) and a spelled candidate (/var/...)
    // name the same directory through a host-level symlink above the project.
    // Re-spell the root as the candidate's own ancestor that IS that directory,
    // then check every component below it. Components inside the root are never
    // skipped, so in-project symlink aliases are still denied.
    fs::path root = root_in;
    const auto absolute = fs::absolute(original).lexically_normal();
    if (!lexically_under(absolute, root)) {
        for (auto a = absolute.parent_path(); !a.empty(); a = a.parent_path()) {
            std::error_code eq_error;
            if (fs::equivalent(a, root_in, eq_error) && !eq_error) { root = a; break; }
            if (a == a.root_path()) break;
        }
    }
    const auto relative = checked_relative_source_path(absolute, root);
    auto next = root;
    for (const auto& component : relative) {
        next /= component;
        std::error_code error;
        const auto status = fs::symlink_status(next, error);
        if (error && error != std::errc::no_such_file_or_directory)
            throw std::runtime_error("cannot safely inspect native source path");
        if (!error && fs::is_symlink(status))
            throw std::runtime_error("native source symlink alias denied");
    }
}
} // namespace logical_source_detail

// Build-scoped logical-to-physical identity registry. No global or cross-tenant
// state, no ambient external roots, no new macro authority.
class NativeLogicalSourceAdmission {
public:
    explicit NativeLogicalSourceAdmission(const std::filesystem::path& entry)
        : root_(logical_source_detail::root_for_entry(entry)),
          spelled_root_(logical_source_detail::spelled_root_for_entry(entry)) {}

    const std::filesystem::path& root() const { return root_; }

    std::string admit(const std::filesystem::path& path,
                      const SourceIdentity& identity) {
        namespace fs = std::filesystem;
        const auto absolute = fs::absolute(path).lexically_normal();
        // Imports resolved through canonical() arrive spelled canonically; others
        // keep the entry's spelling. Check components against the matching root.
        logical_source_detail::no_symlink_components(absolute,
            logical_source_detail::lexically_under(absolute, spelled_root_) ? spelled_root_ : root_);
        const auto physical = fs::canonical(absolute);
        if (!logical_source_detail::under(root_, physical))
            throw std::runtime_error("native source escapes project ownership root");
        std::error_code error;
        if (!fs::is_regular_file(physical, error) || error)
            throw std::runtime_error("native source must be a regular file");
        const auto links = fs::hard_link_count(physical, error);
        if (error || links != 1)
            throw std::runtime_error("native hardlink source aliases are not admitted");
        if (!identity.valid)
            throw std::runtime_error("native source lacks verified descriptor identity");
        const auto relative = physical.lexically_relative(root_).generic_string();
        const auto logical = std::string("project/") + relative;
        logical_source_detail::validate_logical_id(logical);
        for (const auto& prior : entries_) {
            if (prior.logical == logical) {
                if (prior.physical != physical ||
                    !ores_source_identity_equal(prior.identity, identity))
                    throw std::runtime_error("duplicate native logical source ID");
                return logical; // legal cycle/back-edge, no new unit charge
            }
            if ((prior.identity.valid && prior.identity.device == identity.device &&
                 prior.identity.inode == identity.inode && prior.identity.device != 0) ||
                fs::equivalent(prior.physical, physical))
                throw std::runtime_error("native source file alias has multiple logical IDs");
        }
        if (entries_.size() >= 128)
            throw std::runtime_error("native source identity registry exceeds 128 files");
        entries_.push_back({logical, physical, identity});
        return logical;
    }

private:
    struct Entry {
        std::string logical;
        std::filesystem::path physical;
        SourceIdentity identity;
    };
    std::filesystem::path root_;
    std::filesystem::path spelled_root_;
    std::vector<Entry> entries_;
};

} // namespace frontend
} // namespace oreslang
