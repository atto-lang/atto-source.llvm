#pragma once

#include <stdexcept>
#include <string_view>

namespace oreslang {

// The experimental numeric profile must never silently downgrade to legacy
// wrapping because its directive is malformed, displaced, duplicated or
// requests an unsupported version. Ordinary unmarked source remains legacy.
inline bool checked_i64_source_opt_in(std::string_view source) {
    constexpr std::string_view directive_prefix = "// oreslang:checked-i64";
    constexpr std::string_view directive_line = "// oreslang:checked-i64-v1";
    constexpr std::string_view exact_header = "// oreslang:checked-i64-v1\n";
    const bool enabled = source.starts_with(exact_header);

    for (std::size_t pos = 0; pos < source.size();) {
        const std::size_t end = source.find('\n', pos);
        const std::size_t stop = end == std::string_view::npos ? source.size() : end;
        const auto line = source.substr(pos, stop - pos);
        std::size_t offset = 0;
        // UTF-8 BOM is not a valid checked profile prefix.
        if (pos == 0 && line.starts_with("\xEF\xBB\xBF")) offset = 3;
        // Mirror the native lexer's ASCII whitespace admission. CR, VT and
        // FF before a directive must never silently select wrapping i64.
        while (offset < line.size() &&
               (line[offset] == ' ' || line[offset] == '\t' ||
                line[offset] == '\r' || line[offset] == '\v' ||
                line[offset] == '\f')) ++offset;
        if (line.substr(offset).starts_with(directive_prefix) &&
            (!enabled || pos != 0 || offset != 0 || line != directive_line)) {
            throw std::runtime_error(
                "checked-i64-v1: malformed, duplicated, displaced or unsupported directive");
        }
        if (end == std::string_view::npos) break;
        pos = end + 1;
    }
    // A displaced directive after code or a closed block comment is still a
    // real line comment. Detect it before native parsing can silently select
    // the legacy wrapping profile. Ignore quoted source text and nested block
    // comments, neither of which is a line-comment directive.
    unsigned block_depth = 0;
    char quote = '\0';
    for (std::size_t i = 0; i < source.size();) {
        const char c = source[i];
        const char next = i + 1 < source.size() ? source[i + 1] : '\0';
        if (quote != '\0') {
            if (c == '\\' && i + 1 < source.size()) { i += 2; continue; }
            if (c == quote) quote = '\0';
            ++i;
            continue;
        }
        if (block_depth != 0) {
            if (c == '/' && next == '*') { ++block_depth; i += 2; continue; }
            if (c == '*' && next == '/') { --block_depth; i += 2; continue; }
            ++i;
            continue;
        }
        if (c == '"' || c == '\'') { quote = c; ++i; continue; }
        if (c == '/' && next == '*') { block_depth = 1; i += 2; continue; }
        if (c == '/' && next == '/') {
            if (i != 0 && source.substr(i).starts_with(directive_prefix)) {
                throw std::runtime_error(
                    "checked-i64-v1: displaced directive after source tokens");
            }
            const auto end = source.find('\n', i);
            i = end == std::string_view::npos ? source.size() : end + 1;
            continue;
        }
        ++i;
    }

    return enabled;
}

} // namespace oreslang
