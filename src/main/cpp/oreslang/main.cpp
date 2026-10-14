#include "oreslang/backend/LlvmCodegen.hpp"
#include "oreslang/frontend/Parser.hpp"
#include "oreslang/runtime/TargetAbi.hpp"

#include <cstdlib>
#include <fstream>
#include <string_view>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>


// flags-2-env exports these variables using .cli-flags.toml. Read them without
// depending on an ambient TOML parser in the compiler (the upstream wrapper
// owns TOML/argv validation). Invalid inherited values always fail closed.
static bool read_env_flag(const char* key, bool& output) {
    const char* raw = std::getenv(key);
    if (raw == nullptr || *raw == '\0') return true;
    const std::string_view value(raw);
    if (value == "true" || value == "1" || value == "yes") { output = true; return true; }
    if (value == "false" || value == "0" || value == "no") { output = false; return true; }
    std::cerr << "invalid boolean environment value for " << key << '\n';
    return false;
}

int main(int argc, char** argv) {
    bool experimental_owned_int = false;
    bool experimental_rt_scalar_loop = false;
    bool wasm32_target = false;
    bool target_seen = false;
    if (!read_env_flag("ORES_LLVM_EXPERIMENTAL_OWNED_INT", experimental_owned_int)
            || !read_env_flag("ORES_LLVM_EXPERIMENTAL_RT_SCALAR_LOOP", experimental_rt_scalar_loop)) {
        return 2;
    }
    const char* env_target = std::getenv("ORES_LLVM_TARGET");
    std::string target_value = env_target ? env_target : "host";
    const char* env_cpu = std::getenv("ORES_LLVM_CPU");
    if (env_cpu && *env_cpu) {
        std::cerr << "explicit CPU feature selection not implemented: " << env_cpu << '\n';
        return 2;
    }
    bool owned_seen = false;
    bool scalar_seen = false;
    const char* path = nullptr;
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (arg == "--experimental-rt-scalar-loop" || arg == "--no-experimental-rt-scalar-loop") {
            if (scalar_seen) {
                std::cerr << "duplicate rt scalar loop option\n"; return 2;
            }
            scalar_seen = true;
            experimental_rt_scalar_loop = arg == "--experimental-rt-scalar-loop";
        } else if (arg == "--experimental-owned-int" || arg == "--no-experimental-owned-int") {
            if (owned_seen) { std::cerr << "duplicate owned-int option\n"; return 2; }
            owned_seen = true;
            experimental_owned_int = arg == "--experimental-owned-int";
        } else if (arg.rfind("--target=", 0) == 0) {
            if (target_seen) { std::cerr << "duplicate target option\n"; return 2; }
            target_seen = true;
            target_value = arg.substr(9);
        } else if (arg.rfind("--cpu=", 0) == 0) {
            std::cerr << "explicit CPU feature selection not implemented: " << arg << '\n';
            return 2;
        } else if (!arg.empty() && arg[0] != '-' && path == nullptr) path = argv[i];
        else { std::cerr << "unrecognized or duplicate option: " << arg << '\n'; return 2; }
    }
    if (target_value == "host") {
        // Default target and ABI are inferred downstream.
    } else if (target_value == "wasm32" || target_value == "wasm32-unknown-unknown") {
        wasm32_target = oreslang::TargetAbi::parse("wasm32-unknown-unknown").experimental_wasm32_ir();
    } else {
        try {
            (void)oreslang::TargetAbi::parse(target_value);
            std::cerr << "target has no certified IR/ABI lowering: " << target_value << '\n';
        } catch (const std::invalid_argument&) {
            std::cerr << "unknown target triple: " << target_value << '\n';
        }
        return 2;
    }
    if (path == nullptr) {
        std::cerr << "usage: oreslang-llvmc [--target=host|wasm32|wasm32-unknown-unknown] [--experimental-owned-int] [--experimental-rt-scalar-loop] <source.ores>\n"
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
        std::cout << oreslang::emit_verified_llvm_ir(ast, wasm32_target);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "oreslang-llvmc: " << error.what() << '\n';
        return 1;
    }
}
