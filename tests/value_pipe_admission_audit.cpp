// Parser-stage admission audit; tagged values are deliberately not lowered.
#include "oreslang/frontend/Parser.hpp"
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

struct Test { const char* name; std::string snippet; bool admit; const char* expected; };
int main() {
    const std::string functions =
        "fnc inc(int n): int { return n + 1; }\n"
        "fnc add(int n, int k): int { return n + k; }\n";
    const char* tag = "native tagged Option/Result pipeline unsupported";
    const std::vector<Test> cases = {
        {"scalar", "return 2 |> inc |> add(4);", true, ""},
        {"comment", "return 2 |> /*gap*/ inc;", true, ""},
        {"result", "return 1 |> Result.map_err(inc);", false, tag},
        {"option", "return 1 |> Option.map(inc);", false, tag},
        {"option_comment", "return 1 |> Option /*gap*/ . map(inc);", false, tag},
        {"result_comment", "return 1 |> Result //gap\n. map_err(inc);", false, tag},
        {"chained", "return 1 |> inc |> Result.map_err(inc);", false, tag},
        {"nested", "return 1 |> add(3 |> Option.map(inc));", false, tag},
        {"integer_rhs", "return 1 |> 2;", false, "native pipeline stage requires a named scalar callable"},
        {"parenthesized_rhs", "return 1 |> (inc);", false, "native pipeline stage requires a named scalar callable"},
        {"split_token", "return 1 | > inc;", false, ""}
    };
    for (const auto& test : cases) {
        const std::string source = functions + "pub fnc main(): int { " + test.snippet + " }";
        bool accepted = true;
        std::string error;
        try { (void)oreslang::parse_program(source); }
        catch (const std::exception& e) { accepted = false; error = e.what(); }
        if (accepted != test.admit ||
            (!accepted && error.find(test.expected) == std::string::npos)) {
            std::cerr << "Unexpected parser result for " << test.name << ": " << error << "\n";
            return 1;
        }
    }
    for (unsigned n : {0u, 1u, 127u, 128u, 129u, 200u}) {
        std::string source = functions + "pub fnc main(): int { return 1";
        for (unsigned i = 0; i < n; ++i) source += " |> inc";
        source += "; }";
        bool accepted = true;
        std::string error;
        try { (void)oreslang::parse_program(source); }
        catch (const std::exception& e) { accepted = false; error = e.what(); }
        if (accepted != (n <= 128) ||
            (n > 128 && error.find("pipeline exceeds 128 stages") == std::string::npos)) {
            std::cerr << "Unexpected pipeline depth " << n << ": " << error << "\n";
            return 2;
        }
    }
    std::cout << "PASS: 17 native parser admission and depth cases\n";
}
