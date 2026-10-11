#include "oreslang/backend/LlvmCodegen.hpp"
#include "oreslang/frontend/Parser.hpp"
#include "oreslang/frontend/ClientContractHeader.hpp"

#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

int main(int argc, char** argv) {
    const bool check_contract = argc == 3 && std::string(argv[1]) == "--check-contract";
    const bool emit_contract = argc == 3 && std::string(argv[1]) == "--emit-contract-llvm";
    const bool experimental_owned_int =
        argc == 3 && std::string(argv[1]) == "--experimental-owned-int";
    if (argc != 2 && !experimental_owned_int && !check_contract && !emit_contract) {
        std::cerr << "usage: oreslang-llvmc [--experimental-owned-int|--check-contract|--emit-contract-llvm] <source.ores>\n"
                  << "Supported: int functions/parameters, return arithmetic, and same-module calls.\n";
        return 2;
    }
    try {
        const char* path = (experimental_owned_int || check_contract || emit_contract) ? argv[2] : argv[1];
        std::ifstream source(path, std::ios::binary);
        if (!source.is_open()) {
            throw std::runtime_error(std::string("cannot open Oreslang source: ") + path);
        }
        const std::string text((std::istreambuf_iterator<char>(source)),
                               std::istreambuf_iterator<char>());
        if (source.bad()) throw std::runtime_error("failed reading Oreslang source");
        // Target/capability directives are lexical comments in the native
        // subset. Reject rather than silently discard unverified portability
        // constraints before parsing or emitting LLVM IR.
        if (text.find("// ores:") != std::string::npos) {
            throw std::runtime_error("native target admission unsupported: ores environment headers require compiler verification");
        }
        if (check_contract || emit_contract) {
            const auto contract = oreslang::parse_client_contract_header(text);
            if (emit_contract) {
                std::cout << oreslang::emit_verified_client_contract_llvm_ir(contract);
            } else {
                std::cout << "checked Oreslang contract " << contract.name
                          << " signatures=" << contract.methods.size() << '\n';
            }
            return 0;
        }
        auto ast = oreslang::parse_program(text, experimental_owned_int);
        std::cout << oreslang::emit_verified_llvm_ir(ast);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "oreslang-llvmc: " << error.what() << '\n';
        return 1;
    }
}
