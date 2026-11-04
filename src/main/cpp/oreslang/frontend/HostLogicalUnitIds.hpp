#pragma once
// Compiler-owned host path -> logical unit identity admission (M0).
// This does not authorize imported macros or replace descriptor-pinned reads.
#include <cstdint>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace oreslang::frontend {
namespace fs = std::filesystem;

inline bool is_disallowed_format_codepoint(std::uint32_t cp) {
    // Java Character.FORMAT policy mirror for currently catalogued Cf ranges.
    // Updating Unicode character properties needs a versioned conformance test.
    return cp == 0x00ad || (cp >= 0x0600 && cp <= 0x0605) ||
        cp == 0x061c || cp == 0x06dd || cp == 0x070f ||
        (cp >= 0x0890 && cp <= 0x0891) || cp == 0x08e2 ||
        cp == 0x180e || (cp >= 0x200b && cp <= 0x200f) ||
        (cp >= 0x202a && cp <= 0x202e) ||
        (cp >= 0x2060 && cp <= 0x206f) ||
        cp == 0xfeff || (cp >= 0xfff9 && cp <= 0xfffb) ||
        cp == 0x110bd || cp == 0x110cd ||
        (cp >= 0x13430 && cp <= 0x1343f) ||
        (cp >= 0x1bca0 && cp <= 0x1bca3) ||
        (cp >= 0x1d173 && cp <= 0x1d17a) ||
        cp == 0xe0001 || (cp >= 0xe0020 && cp <= 0xe007f);
}

struct SourceStamp {
    std::uint64_t device = 0, inode = 0, size = 0;
    std::int64_t mtime_sec = 0, mtime_nsec = 0;
    std::int64_t ctime_sec = 0, ctime_nsec = 0;
    bool valid = false;
};

inline bool same_source_stamp(const SourceStamp& a, const SourceStamp& b) {
    return a.valid && b.valid && a.device == b.device &&
        a.inode == b.inode && a.size == b.size &&
        a.mtime_sec == b.mtime_sec && a.mtime_nsec == b.mtime_nsec &&
        a.ctime_sec == b.ctime_sec && a.ctime_nsec == b.ctime_nsec;
}

inline void validate_logical_unit_id(const std::string& id) {
    if (id.empty() || id.size() > 4096 || id.front() == '/' || id.back() == '/' || id.find(':') != std::string::npos)
        throw std::runtime_error("ATTO_UNIT_ID_INVALID: empty, absolute, drive or oversized identity");
    for (std::size_t p = 0; p < id.size();) {
        const auto c = static_cast<unsigned char>(id[p]);
        std::uint32_t cp = 0;
        std::size_t count = 0;
        if (c < 0x80) { cp = c; count = 1; }
        else if (c >= 0xc2 && c <= 0xdf) { cp = c & 0x1f; count = 2; }
        else if (c >= 0xe0 && c <= 0xef) { cp = c & 0x0f; count = 3; }
        else if (c >= 0xf0 && c <= 0xf4) { cp = c & 0x07; count = 4; }
        else throw std::runtime_error("ATTO_UNIT_ID_INVALID: malformed UTF-8");
        if (p + count > id.size()) throw std::runtime_error("ATTO_UNIT_ID_INVALID: truncated UTF-8");
        for (std::size_t k = 1; k < count; ++k) {
            const auto b = static_cast<unsigned char>(id[p+k]);
            if ((b & 0xc0) != 0x80) throw std::runtime_error("ATTO_UNIT_ID_INVALID: malformed UTF-8");
            cp = (cp << 6) | (b & 0x3f);
        }
        if ((count == 2 && cp < 0x80) || (count == 3 && cp < 0x800) ||
            (count == 4 && cp < 0x10000) || (cp >= 0xd800 && cp <= 0xdfff) ||
            cp > 0x10ffff || cp < 0x20 || cp == 0x7f ||
            (cp >= 0x80 && cp <= 0x9f) ||
            is_disallowed_format_codepoint(cp) ||
            cp == 0x2028 || cp == 0x2029)
            throw std::runtime_error("ATTO_UNIT_ID_INVALID: control or spoofing codepoint");
        p += count;
    }
    // Reject dot components rather than normalizing away attacker-provided spelling.
    std::size_t start = 0;
    while (start < id.size()) {
        auto end = id.find('/', start);
        if (end == std::string::npos) end = id.size();
        const auto segment = id.substr(start, end-start);
        if (segment.empty() || segment == "." || segment == "..")
            throw std::runtime_error("ATTO_UNIT_ID_INVALID: noncanonical path component");
        start = end + 1;
    }
}

class HostLogicalUnitIds {
    fs::path root_;
    std::map<std::string, fs::path> owner_by_id_;
    std::map<fs::path, std::string> id_by_path_;
    std::map<std::string, SourceStamp> stamps_;
public:
    explicit HostLogicalUnitIds(const fs::path& declared_root)
        : root_(fs::canonical(declared_root)) {
        if (!fs::is_directory(root_))
            throw std::runtime_error("ATTO_UNIT_ROOT_INVALID");
    }

    std::string admit(const fs::path& source_file) {
        const auto lexical = fs::absolute(source_file).lexically_normal();
        const auto relative = lexical.lexically_relative(root_);
        if (relative.empty() || relative.is_absolute())
            throw std::runtime_error("ATTO_UNIT_ROOT_ESCAPE");
        for (const auto& part : relative)
            if (part == ".." || part == ".")
                throw std::runtime_error("ATTO_UNIT_ROOT_ESCAPE");
        // No source symlink, even if it remains within the project.
        fs::path cursor = root_;
        for (const auto& part : relative) {
            cursor /= part;
            if (fs::is_symlink(fs::symlink_status(cursor)))
                throw std::runtime_error("ATTO_UNIT_SOURCE_ALIAS: symlink");
        }
        // A nested manifest represents a different package owner. Relative
        // imports may not silently borrow that package's logical identity.
        for (fs::path parent = lexical.parent_path(); parent != root_;
                parent = parent.parent_path()) {
            if (parent.empty() || parent == parent.root_path())
                throw std::runtime_error("ATTO_UNIT_ROOT_ESCAPE");
            for (const char* marker : {".atto.pkg.toml", ".ores.pkg.toml"}) {
                std::error_code error;
                const auto status = fs::symlink_status(parent / marker, error);
                if (error && error != std::errc::no_such_file_or_directory)
                    throw std::runtime_error("ATTO_UNIT_NESTED_PACKAGE: unreadable package marker");
                if (!error && status.type() != fs::file_type::not_found)
                    throw std::runtime_error("ATTO_UNIT_NESTED_PACKAGE");
            }
        }
        const auto physical = fs::canonical(lexical);
        if (physical != lexical || !fs::is_regular_file(physical))
            throw std::runtime_error("ATTO_UNIT_SOURCE_ALIAS: nonregular or redirected source");
        if (fs::hard_link_count(physical) > 1)
            throw std::runtime_error("ATTO_UNIT_SOURCE_ALIAS: hardlink");
        const auto id = std::string("project/") + relative.generic_string();
        validate_logical_unit_id(id);
        const auto old = owner_by_id_.find(id);
        if (old != owner_by_id_.end() && old->second != physical)
            throw std::runtime_error("ATTO_UNIT_ID_COLLISION");
        for (const auto& entry : id_by_path_)
            if (entry.first != physical && fs::equivalent(entry.first, physical))
                throw std::runtime_error("ATTO_UNIT_SOURCE_ALIAS: duplicate filesystem object");
        const auto previous = id_by_path_.find(physical);
        if (previous != id_by_path_.end() && previous->second != id)
            throw std::runtime_error("ATTO_UNIT_ID_COLLISION");
        owner_by_id_.emplace(id, physical);
        id_by_path_.emplace(physical, id);
        return id;
    }

    void bind_opened_source(const std::string& logical_id, SourceStamp observed) {
        if (!observed.valid || !owner_by_id_.count(logical_id))
            throw std::runtime_error("ATTO_UNIT_SOURCE_UNVERIFIED");
        const auto earlier = stamps_.find(logical_id);
        if (earlier != stamps_.end() &&
                !same_source_stamp(earlier->second, observed))
            throw std::runtime_error("ATTO_UNIT_SOURCE_CHANGED");
        for (const auto& entry : stamps_) {
            if (entry.first != logical_id &&
                    entry.second.device == observed.device &&
                    entry.second.inode == observed.inode)
                throw std::runtime_error("ATTO_UNIT_SOURCE_ALIAS: duplicate opened inode");
        }
        stamps_.emplace(logical_id, observed);
    }
};
} // namespace oreslang::frontend
