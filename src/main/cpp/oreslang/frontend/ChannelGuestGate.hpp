#pragma once
#include <cstddef>
#include <string_view>

namespace oreslang::frontend {

// Recognize only the exact compiler-owned Channel<int> type suffix after the
// canonical native lexer has emitted a Channel identifier. This function is
// NOT a source scanner: strings, comments and templates must remain opaque
// under Lexer::next(). No host or LLVM channel execution is authorized here.
inline bool looks_like_channel_int(std::string_view source,
                                   std::size_t after_channel) noexcept {
    auto skip_space = [&](std::size_t& cursor) {
        while (cursor < source.size()) {
            const char c = source[cursor];
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n'
                    && c != '\v' && c != '\f') break;
            ++cursor;
        }
    };
    std::size_t at = after_channel;
    skip_space(at);
    if (at == source.size() || source[at] != '<') return false;
    ++at;
    skip_space(at);
    if (source.substr(at, 3) != "int") return false;
    at += 3;
    // intSuffix is a different, unsupported type. Do not treat prefix matches
    // as built-in Channel<int> instances or guess generic arguments.
    if (at < source.size() &&
        ((source[at] >= 'a' && source[at] <= 'z') ||
         (source[at] >= 'A' && source[at] <= 'Z') ||
         (source[at] >= '0' && source[at] <= '9') ||
         source[at] == '_')) return false;
    skip_space(at);
    return at < source.size() && source[at] == '>';
}

} // namespace oreslang::frontend
