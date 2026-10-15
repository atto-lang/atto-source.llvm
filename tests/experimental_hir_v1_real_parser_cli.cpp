// Test-only executable wrapper around the REAL native parser and HIR exporter.
// NOT the main oreslang-llvmc binary, codegen, or a production compiler profile.
// Used by private exact-source HIR parity tests with the Java frontend.
#include "oreslang/frontend/Parser.hpp"
#include "oreslang/frontend/ExperimentalHirV1.hpp"
#include "oreslang/frontend/CheckedI64Directive.hpp"
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

int main(int argc, char** argv) {
    if (argc != 3 || std::string(argv[1]) != "--experimental-emit-hir-v1") {
        std::cerr << "usage: experimental_hir_v1_real_parser_cli "
                     "--experimental-emit-hir-v1 <source.ores>\n";
        return 2;
    }
    try {
        std::ifstream in(argv[2], std::ios::binary);
        if (!in) throw std::runtime_error("source file unavailable");
        const std::string source(std::istreambuf_iterator<char>{in},
                                 std::istreambuf_iterator<char>{});
        if (in.bad()) throw std::runtime_error("source read failed");
        if (source.size() > 1'048'576) throw std::runtime_error("source exceeds 1 MiB");
        const bool checked = oreslang::checked_i64_source_opt_in(source);
        std::cout << oreslang::emit_experimental_hir_v1(
            oreslang::parse_program(source), checked);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "experimental HIR v1 real-parser CLI: " << error.what() << '\n';
        return 1;
    }
}
