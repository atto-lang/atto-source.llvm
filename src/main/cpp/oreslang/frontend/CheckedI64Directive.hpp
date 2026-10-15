#pragma once

#include <stdexcept>
#include <string_view>

namespace oreslang {

// The experimental numeric profile must never silently downgrade to legacy
// wrapping because its directive is malformed, displaced, duplicated or
// requests an unsupported version. Ordinary unmarked source remains legacy.
inline bool checked_i64_source_opt_in(std::string_view source) {
    constexpr std::string_view directive_prefix = "// oreslang:checked-i64";
    constexpr std::string_view exact_header = "// oreslang:checked-i64-v1\n";
    const bool enabled = source.starts_with(exact_header);

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
            // Only a real line-comment token may carry a checked-profile
            // directive. The previous line-oriented prepass also matched
            // lines inside nested block comments, rejecting valid unmarked
            // source. This lexical gate rejects malformed first-line forms,
            // duplicates, and displaced directives without that false positive.
            if (source.substr(i).starts_with(directive_prefix) &&
                (!enabled || i != 0)) {
                throw std::runtime_error(
                    "checked-i64-v1: malformed, duplicated, displaced or unsupported directive");
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
