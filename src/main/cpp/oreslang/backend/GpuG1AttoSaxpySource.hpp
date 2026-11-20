#pragma once

#include "oreslang/backend/GpuG1NvptxSaxpyIr.hpp"
#include <array>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace oreslang::gpu::g1 {

// Bounded, typed GPU-G1 frontend island, not general AttoLang GPU semantics.
// It proves a single SAXPY operation, including safe alpha-renamed bindings,
// before invoking the immutable device-IR backend. Host launch is rejected.
struct G1SourceResult {
    std::string llvm_ir;
    std::size_t tokens = 0;
};

enum class SourceType : unsigned char { F32, F32ReadBuffer, F32WriteBuffer, U64 };
struct SourceParameter {
    SourceType type;
    std::string name;
};
struct SaxpySourceAst {
    std::array<SourceParameter, 5> parameters;
    std::string lane;
    bool terminal_return = false;
};

inline bool source_identifier_start(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
inline bool source_decimal(char c) noexcept { return c >= '0' && c <= '9'; }
inline bool source_identifier_part(char c) noexcept {
    return source_identifier_start(c) || source_decimal(c);
}

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
                   source_identifier_part(source[i])) ++i;
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

class SaxpySourceParser final {
public:
    explicit SaxpySourceParser(const std::vector<std::string>& tokens)
        : tokens_(tokens) {}

    SaxpySourceAst parse() {
        take("pub"); take("gpu"); take("fnc"); take("saxpy"); take("(");
        SaxpySourceAst ast{};
        ast.parameters[0] = {SourceType::F32, bind("f32")}; take(",");
        ast.parameters[1] = {SourceType::F32ReadBuffer, bind_buffer(false)}; take(",");
        ast.parameters[2] = {SourceType::F32ReadBuffer, bind_buffer(false)}; take(",");
        ast.parameters[3] = {SourceType::F32WriteBuffer, bind_buffer(true)}; take(",");
        ast.parameters[4] = {SourceType::U64, bind("u64")};
        take(")"); take("="); take(">"); take("void"); take("{");
        take("const"); take("u64"); ast.lane = identifier();
        for (const auto& parameter : ast.parameters)
            if (parameter.name == ast.lane)
                fail("GPU-G1-SOURCE-004: lane shadows a kernel parameter");
        take("="); take("gpu"); take("."); take("global_id");
        take("("); take("0"); take(")"); take(";");
        take("if"); take(ast.lane); take(">="); take(ast.parameters[4].name);
        take("{"); take("return"); take(";"); take("}");
        take(ast.parameters[3].name); index(ast.lane); take("=");
        take(ast.parameters[0].name); take("*");
        take(ast.parameters[1].name); index(ast.lane); take("+");
        take(ast.parameters[2].name); index(ast.lane); take(";");
        if (peek() == "return") {
            take("return"); take(";"); ast.terminal_return = true;
        }
        take("}");
        if (position_ != tokens_.size())
            fail("GPU-G1-SOURCE-002: trailing declaration or host launch forbidden");
        return ast;
    }

private:
    const std::vector<std::string>& tokens_;
    std::size_t position_ = 0;
    std::vector<std::string> names_;

    [[noreturn]] static void fail(const char* error) {
        throw std::invalid_argument(error);
    }
    std::string_view peek() const noexcept {
        return position_ < tokens_.size() ? std::string_view(tokens_[position_]) : std::string_view{};
    }
    void take(std::string_view expected) {
        if (peek() != expected)
            fail("GPU-G1-SOURCE-003: unproved type, effect or expression");
        ++position_;
    }
    std::string identifier() {
        const auto value = peek();
        if (value.empty() || value.size() > 64 || !source_identifier_start(value.front()))
            fail("GPU-G1-SOURCE-004: invalid kernel binding");
        for (char c : value)
            if (!source_identifier_part(c))
                fail("GPU-G1-SOURCE-004: invalid kernel binding");
        // The restricted frontend has no keyword-as-variable exceptions.
        static constexpr std::string_view reserved[] = {
            "pub", "gpu", "fnc", "const", "let", "mut", "return", "void",
            "if", "else", "await", "suspends", "async", "critical", "rt",
            "new", "spawn", "defer", "recover", "throw", "panic", "true",
            "false", "global_id", "GpuArray", "f32", "u64", "saxpy"
        };
        for (auto keyword : reserved)
            if (value == keyword)
                fail("GPU-G1-SOURCE-004: reserved kernel binding");
        ++position_;
        return std::string(value);
    }
    std::string bind(std::string_view type) {
        if (!type.empty()) take(type);
        std::string name = identifier();
        for (const auto& prior : names_)
            if (prior == name)
                fail("GPU-G1-SOURCE-004: duplicate kernel binding");
        names_.push_back(name);
        return name;
    }
    std::string bind_buffer(bool exclusive) {
        take("GpuArray"); take("<"); take("f32"); take(">");
        if (exclusive) take("mut");
        return bind("");
    }
    void index(std::string_view lane) {
        take("["); take(lane); take("]");
    }
};

inline G1SourceResult compile_g1_saxpy_source(std::string_view source) {
    const auto tokens = tokenize_g1_source(source);
    const auto ast = SaxpySourceParser(tokens).parse();
    // AST slots and checked lexical equality establish the sole supported
    // scalar dataflow. Runtime allocation/queue aliasing remains a separate
    // provider responsibility. Fixed backend ABI prevents source injection.
    if (ast.parameters[0].type != SourceType::F32 ||
        ast.parameters[1].type != SourceType::F32ReadBuffer ||
        ast.parameters[2].type != SourceType::F32ReadBuffer ||
        ast.parameters[3].type != SourceType::F32WriteBuffer ||
        ast.parameters[4].type != SourceType::U64)
        throw std::invalid_argument("GPU-G1-SOURCE-003: invalid typed kernel AST");
    const SaxpyContract checked{};
    return {lower_saxpy_to_nvptx_llvm_ir(checked), tokens.size()};
}

} // namespace oreslang::gpu::g1
