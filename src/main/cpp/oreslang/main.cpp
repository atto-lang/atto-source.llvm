#include "oreslang/backend/LlvmCodegen.hpp"
#include "oreslang/frontend/Parser.hpp"

#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

int main(int argc, char** argv) {
    const bool experimental_owned = argc == 3 && std::string(argv[1]) == "--experimental-owned-int";
    if ((argc != 2 && !experimental_owned) || (argc == 2 && std::string(argv[1]).rfind("--", 0) == 0)) {
        std::cerr << "usage: oreslang-llvmc [--experimental-owned-int] <source.ores>\n"
                  << "Supported: int functions/parameters, return arithmetic, and same-module calls.\n";
        return 2;
    }
    try {
        const char* path = experimental_owned ? argv[2] : argv[1];
        std::ifstream source(path, std::ios::binary);
        if (!source.is_open()) {
            throw std::runtime_error(std::string("cannot open Oreslang source: ") + path);
        }
        const std::string text((std::istreambuf_iterator<char>(source)),
                               std::istreambuf_iterator<char>());
        if (source.bad()) throw std::runtime_error("failed reading Oreslang source");
        oreslang::ParserOptions options;
        options.experimental_owned_int = experimental_owned;
        auto ast = oreslang::parse_program(text, options);
        std::cout << oreslang::emit_verified_llvm_ir(ast);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "oreslang-llvmc: " << error.what() << '\n';
        return 1;
    }
}
