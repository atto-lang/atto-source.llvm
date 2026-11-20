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
                "expanded import specifier contains control characters or display-spoofing characters");
    }
    // Environment values are OS bytes on native platforms. A malformed UTF-8
    // byte sequence must never become a different filesystem identity from
    // Java's Unicode string representation or leak through source receipts.
    for (std::size_t i = 0; i < output.size();) {
        const auto first = static_cast<unsigned char>(output[i]);
        if (first < 0x80) {
            ++i;
            continue;
        }
        std::size_t continuation = 0;
        unsigned char second_min = 0x80;
        unsigned char second_max = 0xBF;
        if (first >= 0xC2 && first <= 0xDF) continuation = 1;
        else if (first >= 0xE0 && first <= 0xEF) {
            continuation = 2;
            if (first == 0xE0) second_min = 0xA0;
            if (first == 0xED) second_max = 0x9F;
        } else if (first >= 0xF0 && first <= 0xF4) {
            continuation = 3;
            if (first == 0xF0) second_min = 0x90;
            if (first == 0xF4) second_max = 0x8F;
        } else {
            throw std::invalid_argument("expanded import specifier is not valid UTF-8");
        }
        if (continuation >= output.size() - i)
            throw std::invalid_argument("expanded import specifier is not valid UTF-8");
        const auto second = static_cast<unsigned char>(output[i + 1]);
        if (second < second_min || second > second_max)
            throw std::invalid_argument("expanded import specifier is not valid UTF-8");
        for (std::size_t k = 2; k <= continuation; ++k) {
            const auto ch = static_cast<unsigned char>(output[i + k]);
            if (ch < 0x80 || ch > 0xBF)
                throw std::invalid_argument("expanded import specifier is not valid UTF-8");
        }
        i += continuation + 1;
    }
    return output;
}

} // namespace oreslang::frontend
