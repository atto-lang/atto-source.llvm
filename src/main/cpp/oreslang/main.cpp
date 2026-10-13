#include "oreslang/backend/LlvmCodegen.hpp"
#include "oreslang/frontend/Parser.hpp"
#include "oreslang/runtime/TargetAbi.hpp"

#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

int main(int argc, char** argv) {
    bool check_only = false;
    bool experimental_owned_int = false;
    bool experimental_rt_scalar_loop = false;
    bool wasm32_target = false;
    bool target_seen = false;
    const char* path = nullptr;
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (arg == "--check") {
            if (check_only) { std::cerr << "duplicate check option\n"; return 2; }
            check_only = true;
        } else if (arg == "--experimental-rt-scalar-loop") {
            if (experimental_rt_scalar_loop) {
                std::cerr << "duplicate rt scalar loop option\n"; return 2;
            }
            experimental_rt_scalar_loop = true;
        } else if (arg == "--experimental-owned-int") {
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
                    std::cerr << "target has no certified IR/ABI lowering: " << target << '\n';
                } catch (const std::invalid_argument&) {
                    std::cerr << "unknown target triple: " << target << '\n';
                }
                return 2;
            }
        } else if (arg.rfind("--cpu=", 0) == 0) {
            std::cerr << "explicit CPU feature selection not implemented: " << arg << '\n';
            return 2;
        } else if (!arg.empty() && arg[0] != '-' && path == nullptr) path = argv[i];
        else { std::cerr << "unrecognized or duplicate option: " << arg << '\n'; return 2; }
    }
    if (path == nullptr) {
        std::cerr << "usage: oreslang-llvmc [--check] [--target=host|wasm32|wasm32-unknown-unknown] [--experimental-owned-int] [--experimental-rt-scalar-loop] <source.ores>\n"
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
        if (experimental_rt_scalar_loop && (wasm32_target || experimental_owned_int))
            throw std::runtime_error("scalar-loop frame requires native host and no owned-int experiment");
        auto ast = oreslang::parse_program(text, experimental_owned_int,
                                           experimental_rt_scalar_loop);
        if (!ast.client_contracts.empty()) {
            if (!ast.functions.empty() || !ast.classes.empty() || !ast.actors.empty())
                throw std::runtime_error("client contracts must be declaration-only source");
            if (wasm32_target || experimental_owned_int || experimental_rt_scalar_loop)
                throw std::runtime_error("client contract check requires host target without experimental runtime modes");
            if (check_only) return 0;
            throw std::runtime_error("client contracts have no admitted LLVM runtime implementation; use --check for declaration validation");
        }
        auto verified_ir = oreslang::emit_verified_llvm_ir(ast, wasm32_target);
        // Ordinary --check follows the same target/ABI admission path as
        // compilation; parse-only validation must not certify unsupported IR.
        if (!check_only) std::cout << verified_ir;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "oreslang-llvmc: " << error.what() << '\n';
        return 1;
    }
}
