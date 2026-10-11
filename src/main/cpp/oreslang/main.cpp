#include "oreslang/backend/LlvmCodegen.hpp"
#include "oreslang/frontend/Parser.hpp"
#include "oreslang/runtime/TargetAbi.hpp"

#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

int main(int argc, char** argv) {
    bool experimental_owned_int = false;
    bool wasm32_target = false;
    bool target_seen = false;
    const char* path = nullptr;
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (arg == "--experimental-owned-int") {
            if (experimental_owned_int) { std::cerr << "duplicate owned-int option\n"; return 2; }
            experimental_owned_int = true;
        } else if (arg.rfind("--target=", 0) == 0) {
            if (target_seen) { std::cerr << "duplicate target option\n"; return 2; }
            target_seen = true;
            const std::string target = arg.substr(9);
            if (target == "host") {
                // Historical default: host triple and ABI remain inferred downstream.
            } else if (target == "wasm32" || target == "wasm32-unknown-unknown") {
                wasm32_target = oreslang::TargetAbi::parse("wasm32-unknown-unknown").experimental_wasm32_ir();
            } else {
                try {
                    (void)oreslang::TargetAbi::parse(target);
                    std::cerr << "target has no certified IR/ABI lowering: " << target << '\\n';
                } catch (const std::invalid_argument&) {
                    std::cerr << "unknown target triple: " << target << '\\n';
                }
                return 2;
            }
        } else if (arg.rfind("--cpu=", 0) == 0) {
            std::cerr << "explicit CPU feature selection not implemented: " << arg << '\\n';
            return 2;
        } else if (!arg.empty() && arg[0] != '-' && path == nullptr) path = argv[i];
        else { std::cerr << "unrecognized or duplicate option: " << arg << '\n'; return 2; }
    }
    if (path == nullptr) {
        std::cerr << "usage: oreslang-llvmc [--target=host|wasm32|wasm32-unknown-unknown] [--experimental-owned-int] <source.ores>\n"
                  << "Supported: int functions/parameters, return arithmetic, and same-module calls.\n";
        return 2;
    }
    try {
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
        auto ast = oreslang::parse_program(text, experimental_owned_int);
        std::cout << oreslang::emit_verified_llvm_ir(ast, wasm32_target);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "oreslang-llvmc: " << error.what() << '\n';
        return 1;
    }
}
