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
    return enabled;
}

} // namespace oreslang
