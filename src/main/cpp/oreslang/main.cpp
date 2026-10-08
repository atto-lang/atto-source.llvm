#include "oreslang/backend/LlvmCodegen.hpp"
#include "oreslang/frontend/Parser.hpp"

#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: oreslang-llvmc <source.ores>\n"
                  << "Only constant-return, zero-argument int functions are supported.\n";
        return 2;
    }
    try {
        std::ifstream source(argv[1], std::ios::binary);
        if (!source.is_open()) {
            throw std::runtime_error(std::string("cannot open Oreslang source: ") + argv[1]);
        }
        const std::string text(
            (std::istreambuf_iterator<char>(source)),
            std::istreambuf_iterator<char>());
        if (!source.eof() && source.bad()) {
            throw std::runtime_error("failed reading Oreslang source");
        }

        const auto ast = oreslang::parse_constant_function(text);
        std::cout << oreslang::emit_verified_llvm_ir(ast);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "oreslang-llvmc: " << error.what() << '\n';
        return 1;
    }
}
