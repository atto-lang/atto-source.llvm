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
            (cp >= 0x80 && cp <= 0x9f) || (cp >= 0x202a && cp <= 0x202e) ||
            (cp >= 0x2066 && cp <= 0x2069) || cp == 0x200e || cp == 0x200f ||
            cp == 0xfeff || cp == 0x2028 || cp == 0x2029)
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
};
} // namespace oreslang::frontend
