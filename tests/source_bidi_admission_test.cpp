#include "oreslang/frontend/SourceInput.hpp"
#include <stdexcept>
#include <string>
#include <vector>
#include <iostream>

int main() {
    const std::string valid = "pub fnc main(): int { return 42; }";
    const std::vector<std::string> controls = {
        "\xD8\x9C", "\xE2\x80\x8E", "\xE2\x80\x8F",
        "\xE2\x80\xAA", "\xE2\x80\xAB", "\xE2\x80\xAC",
        "\xE2\x80\xAD", "\xE2\x80\xAE",
        "\xE2\x81\xA6", "\xE2\x81\xA7",
        "\xE2\x81\xA8", "\xE2\x81\xA9"
    };
    for (const auto& control : controls) {
        for (const auto& source : {valid + " // " + control, "/* " + control + " */ " + valid}) {
            try {
                require_valid_source_utf8(source);
                std::cerr << "admitted bidirectional formatting control\n";
                return 1;
            } catch (const std::runtime_error& e) {
                if (std::string(e.what()).find("bidirectional Unicode control") == std::string::npos)
                    return 2;
            }
        }
    }
    // Valid UTF-8 (including non-ASCII comments) still reaches the grammar.
    require_valid_source_utf8(valid + " // caf\xC3\xA9");
    return 0;
}
