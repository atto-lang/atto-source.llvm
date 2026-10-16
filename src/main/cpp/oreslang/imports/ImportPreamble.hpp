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
    // Parallel to imports: named public function requested by canonical
    // 'import fnc symbol from "path";' or empty for legacy scalar preamble.
    std::vector<std::string> requested_functions;
};

inline ImportPreamble split_import_preamble(std::string_view source) {
    ImportPreamble output;
    output.stripped_source.reserve(source.size());
    bool declarations_started = false;
    std::size_t at = 0;
    while (at < source.size()) {
        const auto eol = source.find_first_of("\r\n", at);
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
            std::string requested_function;
            // Narrow, executable language syntax parity. This is NOT module
            // namespace linking; only a single named pub fnc is admitted.
            if (text.substr(p, 4) == "fnc " || text.substr(p, 4) == "fnc\t") {
                p += 3;
                while (p < text.size() && (text[p] == ' ' || text[p] == '\t')) ++p;
                const auto name_start = p;
                if (p == text.size() || !((text[p] >= 'a' && text[p] <= 'z') ||
                        (text[p] >= 'A' && text[p] <= 'Z') || text[p] == '_'))
                    throw std::invalid_argument("native named import requires a function identifier");
                while (p < text.size() && ((text[p] >= 'a' && text[p] <= 'z') ||
                        (text[p] >= 'A' && text[p] <= 'Z') ||
                        (text[p] >= '0' && text[p] <= '9') || text[p] == '_')) ++p;
                if (p - name_start > 128)
                    throw std::invalid_argument("native import function identifier too long");
                requested_function = std::string(text.substr(name_start, p - name_start));
                if (p == text.size() || (text[p] != ' ' && text[p] != '\t'))
                    throw std::invalid_argument("native named import requires from");
                while (p < text.size() && (text[p] == ' ' || text[p] == '\t')) ++p;
                if (text.substr(p, 4) != "from" || p + 4 == text.size() ||
                        (text[p + 4] != ' ' && text[p + 4] != '\t'))
                    throw std::invalid_argument("native named import requires from");
                p += 4;
                while (p < text.size() && (text[p] == ' ' || text[p] == '\t')) ++p;
            }
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
            output.requested_functions.push_back(std::move(requested_function));
            output.stripped_source.append(line.size(), ' ');
        } else {
            // Only a contiguous, unambiguous leading import preamble is admitted.
            // An opening block comment ends the preamble; its inner lines must
            // never be mistaken for executable import directives.
            if (!text.empty() && text.substr(0, 2) != "//") declarations_started = true;
            output.stripped_source.append(line);
        }
        if (eol != std::string_view::npos) output.stripped_source.push_back(source[eol]);
        at = eol == std::string_view::npos ? source.size() : eol + 1;
    }
    return output;
}
} // namespace oreslang::imports
