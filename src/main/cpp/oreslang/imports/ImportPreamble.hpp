#pragma once
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace oreslang::imports {
struct ImportPreamble {
    std::string stripped_source;
    std::vector<std::string> imports;
};

inline ImportPreamble split_import_preamble(std::string_view source) {
    ImportPreamble output;
    output.stripped_source.reserve(source.size());
    bool declarations_started = false;
    std::size_t at = 0;
    while (at < source.size()) {
        const auto eol = source.find('\n', at);
        const auto length = (eol == std::string_view::npos ? source.size() : eol) - at;
        const auto line = source.substr(at, length);
        std::size_t first = 0;
        while (first < line.size() && (line[first] == ' ' || line[first] == '\t' || line[first] == '\r')) ++first;
        const auto text = line.substr(first);
        const bool is_import = text.size() > 6 && text.substr(0, 6) == "import" &&
            (text[6] == ' ' || text[6] == '\t');
        if (!declarations_started && is_import) {
            std::size_t p = 6;
            while (p < text.size() && (text[p] == ' ' || text[p] == '\t')) ++p;
            if (p == text.size() || text[p++] != '"')
                throw std::invalid_argument("native import requires a quoted path");
            const auto start = p;
            while (p < text.size() && text[p] != '"') {
                const unsigned char c = static_cast<unsigned char>(text[p]);
                if (c < 32 || c == 127) throw std::invalid_argument("invalid byte in native import");
                ++p;
            }
            if (p == text.size() || p == start)
                throw std::invalid_argument("unterminated or empty native import");
            auto path = std::string(text.substr(start, p - start));
            if (path.size() > 4096) throw std::invalid_argument("native import path too long");
            ++p;
            while (p < text.size() && (text[p] == ' ' || text[p] == '\t')) ++p;
            if (p == text.size() || text[p++] != ';')
                throw std::invalid_argument("native import requires ';'");
            while (p < text.size() && (text[p] == ' ' || text[p] == '\t' || text[p] == '\r')) ++p;
            if (p != text.size() && text.substr(p, 2) != "//")
                throw std::invalid_argument("trailing tokens on native import");
            if (output.imports.size() >= 128)
                throw std::invalid_argument("too many native imports in one source");
            output.imports.push_back(std::move(path));
            output.stripped_source.append(line.size(), ' ');
        } else {
            // Only a contiguous, unambiguous leading import preamble is admitted.
            // An opening block comment ends the preamble; its inner lines must
            // never be mistaken for executable import directives.
            if (!text.empty() && text.substr(0, 2) != "//") declarations_started = true;
            output.stripped_source.append(line);
        }
        if (eol != std::string_view::npos) output.stripped_source.push_back('\n');
        at = eol == std::string_view::npos ? source.size() : eol + 1;
    }
    return output;
}
} // namespace oreslang::imports
