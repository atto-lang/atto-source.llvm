#include "oreslang/backend/LlvmCodegen.hpp"
#include "oreslang/frontend/Parser.hpp"

#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

int main(int argc, char** argv) {
    const bool check_only = argc == 3 && std::string(argv[1]) == "--check";
    if ((!check_only && argc != 2) || (check_only && argc != 3)) {
        std::cerr << "usage: oreslang-llvmc [--check] <source.ores>\n"
                  << "Codegen: native int subset only. --check also parses String client declarations.\n";
        return 2;
    }
    try {
        std::ifstream source(argv[check_only ? 2 : 1], std::ios::binary);
        if (!source.is_open()) {
            throw std::runtime_error(std::string("cannot open Oreslang source: ") + argv[1]);
        }
        const std::string text((std::istreambuf_iterator<char>(source)),
                               std::istreambuf_iterator<char>());
        if (source.bad()) throw std::runtime_error("failed reading Oreslang source");
        auto ast = oreslang::parse_program(text);
        if (check_only) return 0; // syntax/ownership admission only; never emit IR
        std::cout << oreslang::emit_verified_llvm_ir(ast);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "oreslang-llvmc: " << error.what() << '\n';
        return 1;
    }
}
