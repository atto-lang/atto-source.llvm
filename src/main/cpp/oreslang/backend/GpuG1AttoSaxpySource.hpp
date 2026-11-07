#pragma once

#include "oreslang/backend/GpuG1NvptxSaxpyIr.hpp"
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace oreslang::gpu::g1 {

// This deliberately closed source subset is not general AttoLang GPU parsing.
// It proves exactly one .atto saxpy kernel's effect/ABI/body shape before the
// trusted closed device IR emitter is called. Host launch syntax is rejected.
struct G1SourceResult {
    std::string llvm_ir;
    std::size_t tokens = 0;
};

inline bool source_identifier_start(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
inline bool source_decimal(char c) noexcept { return c >= '0' && c <= '9'; }

inline std::vector<std::string> tokenize_g1_source(std::string_view source) {
    if (source.empty() || source.size() > 65536)
        throw std::invalid_argument("GPU-G1-SOURCE-001: source byte quota exceeded");
    std::vector<std::string> tokens;
    tokens.reserve(80);
    for (std::size_t i = 0; i < source.size();) {
        const char c = source[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { ++i; continue; }
        if (source.substr(i, 2) == "//") {
            i += 2;
            while (i < source.size() && source[i] != '\n') ++i;
            continue;
        }
        if (source.substr(i, 2) == "/*") {
            i += 2;
            const auto end = source.find("*/", i);
            if (end == std::string_view::npos ||
                source.substr(i, end - i).find("/*") != std::string_view::npos)
                throw std::invalid_argument("GPU-G1-SOURCE-001: nested or unterminated comment");
            i = end + 2;
            continue;
        }
        const auto start = i;
        if (source_identifier_start(c)) {
            ++i;
            while (i < source.size() &&
                   (source_identifier_start(source[i]) || source_decimal(source[i]))) ++i;
        } else if (source_decimal(c)) {
            ++i;
            while (i < source.size() && source_decimal(source[i])) ++i;
        } else if (source.substr(i, 2) == ">=") {
            i += 2;
        } else if (std::string_view("(){};,<>[]=+*.").find(c) != std::string_view::npos) {
            ++i;
        } else {
            throw std::invalid_argument("GPU-G1-SOURCE-001: unknown token or effect");
        }
        tokens.emplace_back(source.substr(start, i - start));
        if (tokens.size() > 128)
            throw std::invalid_argument("GPU-G1-SOURCE-001: token budget exceeded");
    }
    return tokens;
}

inline G1SourceResult compile_g1_saxpy_source(std::string_view source) {
    const auto tokens = tokenize_g1_source(source);
    // Exact grammar is a temporary *positive kernel-only* acceptance island;
    // no name-based primitive or arbitrary expression/call is admitted.
    // A separately reviewed native GPU parser must eventually replace it.
    static constexpr std::string_view grammar[] = {
        "pub", "gpu", "fnc", "saxpy", "(",
        "f32", "scale", ",", "GpuArray", "<", "f32", ">", "x", ",",
        "GpuArray", "<", "f32", ">", "y", ",",
        "GpuArray", "<", "f32", ">", "mut", "out", ",",
        "u64", "n", ")", "=", ">", "void", "{",
        "const", "u64", "i", "=", "gpu", ".", "global_id", "(", "0", ")", ";",
        "if", "i", ">=", "n", "{", "return", ";", "}",
        "out", "[", "i", "]", "=", "scale", "*", "x", "[", "i", "]",
        "+", "y", "[", "i", "]", ";"
    };
    constexpr std::size_t count = sizeof(grammar) / sizeof(grammar[0]);
    const bool optional_final_return = tokens.size() == count + 3;
    if (tokens.size() != count + 1 && !optional_final_return)
        throw std::invalid_argument("GPU-G1-SOURCE-002: unsupported kernel/launch shape");
    for (std::size_t i = 0; i < count; ++i)
        if (tokens[i] != grammar[i])
            throw std::invalid_argument("GPU-G1-SOURCE-003: unproved device effect, type or body");
    if (optional_final_return &&
        (tokens[count] != "return" || tokens[count + 1] != ";"))
        throw std::invalid_argument("GPU-G1-SOURCE-003: invalid terminal return");
    if (tokens.back() != "}")
        throw std::invalid_argument("GPU-G1-SOURCE-003: trailing source or missing brace");
    // The checked grammar above is the only issuer of this trusted contract.
    // No source text is spliced into LLVM IR; module entry is fixed by ABI.
    const SaxpyContract checked{};
    return {lower_saxpy_to_nvptx_llvm_ir(checked), tokens.size()};
}

} // namespace oreslang::gpu::g1
