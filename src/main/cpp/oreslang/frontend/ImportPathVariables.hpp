#pragma once

#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace oreslang::frontend {

// Resolve only explicit ${NAME} placeholders. $repo has no special meaning,
// and the canonical +/ package-root namespace remains a distinct resolver.
// The supplied lookup represents one compiler environment snapshot; callers
// enforce capability gates, package ownership, source identity and provenance.
inline std::string expand_static_import_path(
        std::string_view raw,
        const std::function<std::optional<std::string>(std::string_view)>& lookup) {
    constexpr std::size_t max_bytes = 4096;
    if (raw.empty() || raw.size() > max_bytes)
        throw std::invalid_argument("invalid import specifier length");
    auto variable = [](std::string_view name) {
        if (name.empty() || name.size() > 128) return false;
        auto first = [](char ch) {
            return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_';
        };
        if (!first(name.front())) return false;
        for (char ch : name)
            if (!(first(ch) || (ch >= '0' && ch <= '9'))) return false;
        return true;
    };
    std::string output;
    output.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size();) {
        if (raw[i] != '$' || i + 1 == raw.size() || raw[i + 1] != '{') {
            output.push_back(raw[i++]);
        } else {
            const auto end = raw.find('}', i + 2);
            if (end == std::string_view::npos)
                throw std::invalid_argument("unterminated ${NAME} import variable");
            const auto name = raw.substr(i + 2, end - i - 2);
            if (!variable(name))
                throw std::invalid_argument("invalid ${NAME} import variable name");
            const auto value = lookup(name);
            if (!value || value->empty())
                throw std::invalid_argument("missing or empty import environment variable: "
                                            + std::string(name));
            if (value->size() > max_bytes || value->find("${") != std::string::npos)
                throw std::invalid_argument("invalid import environment variable value: "
                                            + std::string(name));
            output.append(*value);
            i = end + 1;
        }
        if (output.size() > max_bytes)
            throw std::invalid_argument("expanded import specifier exceeds 4096 bytes");
    }
    // This byte check includes C0, DEL and the UTF-8 encoding of C1 control
    // codepoints. Reject before any filesystem lookup or diagnostic printing.
    for (std::size_t i = 0; i < output.size(); ++i) {
        const auto c = static_cast<unsigned char>(output[i]);
        const auto next = i + 1 < output.size()
                ? static_cast<unsigned char>(output[i + 1]) : 0;
        const auto third = i + 2 < output.size()
                ? static_cast<unsigned char>(output[i + 2]) : 0;
        const bool c1 = c == 0xC2 && next >= 0x80 && next <= 0x9F;
        const bool arabic_letter_mark = c == 0xD8 && next == 0x9C;
        const bool bidi_and_separator = c == 0xE2 &&
                ((next == 0x80 &&
                  (third == 0x8E || third == 0x8F ||
                   (third >= 0xA8 && third <= 0xAE))) ||
                 (next == 0x81 && third >= 0xA6 && third <= 0xA9));
        if (c < 32 || c == 127 || c1 || arabic_letter_mark || bidi_and_separator)
            throw std::invalid_argument(
                "expanded import specifier contains control or display-spoofing characters");
    }
    return output;
}

} // namespace oreslang::frontend
