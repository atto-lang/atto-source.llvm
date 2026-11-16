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
        {"dispatch-value-match-wildcard-then-default-reject.ores", false,
         "native value match wildcard arm must be last"},
        {"dispatch-do-match-wildcard-then-default-reject.ores", false,
         "native do match wildcard arm must be last"},
        {"dispatch-value-match-duplicate-case-reject.ores", false,
         "duplicate native value match pattern"},
        {"dispatch-do-match-duplicate-case-reject.ores", false,
         "duplicate native do match pattern"},
        {"dispatch-value-structural-field-reject.ores", false,
         "native structural field pattern requires a stack-constructed const class local"},
        {"dispatch-do-structural-field-reject.ores", false,
         "native structural field pattern requires a stack-constructed const class local"},
        {"dispatch-value-record-field-reject.ores", false,
         "native tilde map/record literal unsupported"},
        {"dispatch-do-record-field-reject.ores", false,
         "native tilde map/record literal unsupported"},
        {"dispatch-value-public-class-field-run.ores", true, ""},
        {"dispatch-do-public-class-field-run.ores", true, ""},
        {"dispatch-value-private-class-field-reject.ores", false, "native structural field pattern requires a declared public primitive field"},
        {"dispatch-value-mutable-class-owner-reject.ores", false, "native structural field pattern requires a stack-constructed const class local"},
        {"dispatch-value-structural-mixed-fields-reject.ores", false, "native ordinary structural match cannot prove differently named field arms disjoint"},
        {"dispatch-value-structural-duplicate-reject.ores", false, "duplicate native structural field match pattern"},
        {"dispatch-value-structural-multi-field-reject.ores", false, "native structural field pattern supports exactly one field"},
        {"dispatch-value-structural-wrong-type-reject.ores", false, "native structural field pattern literal type does not match declared field"},
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
        {"dispatch-value-match-default-fallback-run.ores", true, ""},
        {"dispatch-do-match-default-fallback-run.ores", true, ""},
        {"dispatch-value-match-default-duplicate-reject.ores", false,
         "native value match can contain only one default arm"},
        {"dispatch-do-match-default-duplicate-reject.ores", false,
         "native do match can contain only one default arm"},
        {"dispatch-value-match-on-default-reject.ores", false,
         "native value match fallback must use 'default ->' without 'on'"},
        {"dispatch-do-match-on-default-reject.ores", false,
         "native do match fallback must use 'default ->' without 'on'"},
        {"dispatch-value-match-default-guard-reject.ores", false,
         "native value match default arm cannot have a guard or binding"},
        {"dispatch-do-match-default-guard-reject.ores", false,
         "native do match default arm cannot have a guard or binding"}
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
