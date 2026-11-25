#include "oreslang/frontend/Parser.hpp"

#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// Real native source parser-only harness: NOT LLVM codegen, CTest/AOT, or
// ownership/runtime execution proof. Input fixtures are exact source blobs.
static std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("unable to read fixture: " + path);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: atto_match_default_parser_smoke <fixture-dir>\n";
        return 2;
    }
    const std::string root = argv[1];
    struct Expected {
        std::string filename;
        bool accept;
        std::string diagnostic;
    };
    const std::vector<Expected> cases{
        {"dispatch-value-match-default-run.ores", true, ""},
        {"dispatch-do-match-default-run.ores", true, ""},
        {"dispatch-value-match-default-only-run.ores", true, ""},
        {"dispatch-do-match-default-only-run.ores", true, ""},
        {"dispatch-value-match-run.ores", true, ""},
        {"dispatch-do-match-run.ores", true, ""},
        {"dispatch-value-match-default-not-last-reject.ores", false,
         "native value match default arm must be last"},
        {"dispatch-do-match-default-not-last-reject.ores", false,
         "native do match default arm must be last"},
        {"tilde-map-literal-native-reject.ores", false,
         "native tilde map/record literal unsupported"},
        {"tilde-record-literal-native-reject.ores", false,
         "native tilde map/record literal unsupported"},
        {"dispatch-value-match-default-fallthrough-run.ores", true, ""},
        {"dispatch-do-match-default-fallthrough-run.ores", true, ""},
        {"dispatch-value-match-duplicate-pattern-reject.ores", false,
         "duplicate native value match pattern"},
        {"dispatch-do-match-duplicate-pattern-reject.ores", false,
         "duplicate native do match pattern"},
        {"dispatch-value-match-guard-reject.ores", false,
         "native scalar value match binding/guard patterns require dedicated pattern lowering"},
        {"dispatch-do-match-guard-reject.ores", false,
         "native scalar do match binding/guard patterns require dedicated pattern lowering"},
        {"tilde-explicit-record-native-reject.ores", false,
         "native tilde map/record literal unsupported"},
        {"tilde-ambiguous-empty-native-reject.ores", false,
         "native tilde map/record literal unsupported"},
        {"dispatch-value-match-wildcard-then-default-reject.ores", false,
         "native value match wildcard arm must be last"},
        {"dispatch-do-match-wildcard-then-default-reject.ores", false,
         "native do match wildcard arm must be last"},
        {"dispatch-value-match-duplicate-case-reject.ores", false,
         "duplicate native value match pattern"},
        {"dispatch-do-match-duplicate-case-reject.ores", false,
         "duplicate native do match pattern"}
    };
    std::size_t checked = 0;
    for (const auto& c : cases) {
        const std::string file = root + "/" + c.filename;
        bool accepted = false;
        std::string diagnostic;
        try {
            auto parsed = oreslang::parse_program(read_file(file));
            accepted = !parsed.functions.empty();
        } catch (const std::exception& e) {
            diagnostic = e.what();
        }
        if (accepted != c.accept ||
                (!c.accept && diagnostic.find(c.diagnostic) == std::string::npos)) {
            std::cerr << "FAIL " << c.filename << " accepted=" << accepted
                      << " expected=" << c.accept << " diagnostic=" << diagnostic << "\n";
            return 1;
        }
        ++checked;
    }
    std::cout << "ATTO_NATIVE_MATCH_DEFAULT_PARSER_PASS checked=" << checked
              << " GUEST_AOT_NOT_EXECUTED\n";
    return 0;
}
