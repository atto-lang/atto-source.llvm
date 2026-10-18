#pragma once

// Host-only, AOT-ready nullary enum tag layout. DOES NOT enable guest enum
// source parsing or overload dispatch in the native compiler.
#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace oreslang::types::enum_tags {
inline constexpr std::size_t MaxCases = 1024;
struct CaseTag {
    std::string name;
    std::int32_t tag;
};

inline void need(bool valid, const char* message) {
    if (!valid) throw std::invalid_argument(message);
}

inline void simple(const std::string& name) {
    need(!name.empty() && name.size() <= 512, "invalid or oversized enum name");
    const auto initial = [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
    };
    need(initial(name.front()), "invalid enum first character");
    for (char c : name) {
        need(initial(c) || (c >= '0' && c <= '9'),
             "native nullary enum AOT ID requires ASCII lexical name");
    }
}

inline void qualified(const std::string& identity) {
    need(!identity.empty(), "missing resolved nominal enum identity");
    need(identity.size() <= 512, "resolved enum identity exceeds canonical ABI byte limit");
    std::size_t pos = 0;
    unsigned components = 0;
    while (true) {
        const std::size_t end = identity.find('.', pos);
        simple(identity.substr(pos, end == std::string::npos
                                   ? std::string::npos : end - pos));
        ++components;
        need(components <= 16, "too many enum identity components");
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    need(components >= 2, "unqualified enum identity");
}

inline void encodeField(std::string& out, const std::string& text) {
    const auto size = static_cast<std::uint32_t>(text.size());
    for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<char>((size >> shift) & 255u));
    }
    out += text;
}

inline void encodeTag(std::string& out, std::int32_t value) {
    const auto encoded = static_cast<std::uint32_t>(value);
    for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<char>((encoded >> shift) & 255u));
    }
}

class EnumTag32Layout final {
public:
    EnumTag32Layout(std::string nominalId, std::vector<CaseTag> cases)
        : nominalId_(std::move(nominalId)) {
        qualified(nominalId_);
        need(!cases.empty() && cases.size() <= MaxCases,
             "enum nullary case budget exceeded");
        schema_.reserve(128 + cases.size() * 24);
        encodeField(schema_, "ores-nullary-enum-tag32-schema-v1");
        encodeField(schema_, nominalId_);
        encodeField(schema_, std::to_string(cases.size()));
        std::int32_t previous = 0;
        for (const CaseTag& item : cases) {
            simple(item.name);
            need(item.tag > previous, "duplicate/out-of-order or invalid wire tag");
            need(tags_.emplace(item.name, item.tag).second, "duplicate enum case name");
            validTags_.insert(item.tag);
            encodeField(schema_, item.name);
            encodeTag(schema_, item.tag);
            previous = item.tag;
        }
    }

    std::int32_t constantTag(const std::string& caseName) const {
        auto found = tags_.find(caseName);
        need(found != tags_.end(), "unknown statically selected enum case");
        return found->second;
    }
    bool containsWireTag(std::int32_t tag) const {
        return tag > 0 && validTags_.count(tag) != 0;
    }
    std::size_t caseCount() const { return tags_.size(); }
    const std::string& canonicalSchemaBytes() const { return schema_; }
    const std::string& nominalId() const { return nominalId_; }

private:
    const std::string nominalId_;
    std::map<std::string, std::int32_t> tags_;
    std::set<std::int32_t> validTags_;
    std::string schema_;
};
} // namespace oreslang::types::enum_tags
