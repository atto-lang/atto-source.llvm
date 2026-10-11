#pragma once

#include "oreslang/runtime/DopKernelIR.hpp"
#include "oreslang/runtime/QuantumCircuitIR.hpp"
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oreslang::accel_source {

// Strict compile-only accelerator frontend. General Oreslang LLVM parser still
// rejects accelerator execution until GPU/QPU device and actor effects exist.
struct GpuProgram {
    std::string name;
    dop::Kernel kernel;
    dop::OpenClArtifact emit_opencl(dop::GpuTarget target) const {
        return dop::lower_opencl(kernel, target);
    }
};
struct QuantumProgram {
    std::string name;
    quantum::Circuit circuit;
    std::string emit_openqasm3() const { return quantum::lower_openqasm3(circuit); }
};

struct Token {
    std::string text;
    bool identifier;
    bool number;
    std::size_t offset;
};

inline bool word_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
inline bool word_part(char c) { return word_start(c) || (c >= '0' && c <= '9'); }
inline bool digit(char c) { return c >= '0' && c <= '9'; }

inline std::vector<Token> tokenize(std::string_view source) {
    std::vector<Token> result;
    std::size_t i = 0;
    while (i < source.size()) {
        const unsigned char byte = static_cast<unsigned char>(source[i]);
        if (std::isspace(byte)) { ++i; continue; }
        if (source.substr(i, 2) == "//") {
            const auto end = source.find('\n', i + 2);
            i = end == std::string_view::npos ? source.size() : end;
            continue;
        }
        if (source.substr(i, 2) == "/*") {
            const auto end = source.find("*/", i + 2);
            if (end == std::string_view::npos)
                throw std::invalid_argument("unterminated accelerator comment at byte " + std::to_string(i));
            i = end + 2;
            continue;
        }
        const auto first = i;
        if (word_start(source[i])) {
            ++i;
            while (i < source.size() && word_part(source[i])) ++i;
            result.push_back({std::string(source.substr(first, i - first)), true, false, first});
            continue;
        }
        if (digit(source[i])) {
            ++i;
            while (i < source.size() && (digit(source[i]) || source[i] == '_')) ++i;
            if (i < source.size() && source[i] == '.' && i + 1 < source.size() && digit(source[i + 1])) {
                ++i;
                while (i < source.size() && (digit(source[i]) || source[i] == '_')) ++i;
            }
            if (i < source.size() && (source[i] == 'e' || source[i] == 'E')) {
                ++i;
                if (i < source.size() && (source[i] == '+' || source[i] == '-')) ++i;
                if (i == source.size() || !digit(source[i]))
                    throw std::invalid_argument("malformed accelerator float exponent at byte " + std::to_string(first));
                while (i < source.size() && (digit(source[i]) || source[i] == '_')) ++i;
            }
            result.push_back({std::string(source.substr(first, i - first)), false, true, first});
            continue;
        }
        if (std::string_view("(){}<>,:;*+-").find(source[i]) != std::string_view::npos) {
            result.push_back({std::string(source.substr(i, 1)), false, false, i});
            ++i;
            continue;
        }
        throw std::invalid_argument("unsupported accelerator symbol at byte " + std::to_string(i));
    }
    result.push_back({"<eof>", false, false, source.size()});
    return result;
}

class Cursor {
public:
    explicit Cursor(std::string_view source) : tokens_(tokenize(source)) {}
    const Token& current() const { return tokens_.at(at_); }
    bool check(std::string_view value) const { return current().text == value; }
    bool take(std::string_view value) {
        if (!check(value)) return false;
        ++at_;
        return true;
    }
    void expect(std::string_view value) {
        if (!take(value)) fail("expected '" + std::string(value) + "'");
    }
    std::string identifier() {
        if (!current().identifier) fail("expected identifier");
        const std::string result = current().text;
        ++at_;
        return result;
    }
    float f32() {
        const bool negative = take("-");
        if (!current().number) fail("expected finite f32 coefficient");
        std::string literal;
        for (char c : current().text) if (c != '_') literal += c;
        ++at_;
        errno = 0;
        char* end = nullptr;
        const float value = std::strtof(literal.c_str(), &end);
        if (end == literal.c_str() || *end != '\0' || errno == ERANGE || !std::isfinite(value))
            fail("invalid finite f32 coefficient");
        return negative ? -value : value;
    }
    int index() {
        if (!current().number) fail("expected unsigned register index");
        const std::string spelling = current().text;
        if (spelling.find_first_of(".eE") != std::string::npos)
            fail("expected integer register index");
        std::string literal;
        for (char c : spelling) if (c != '_') literal += c;
        ++at_;
        if (literal.size() > 10) fail("invalid register index");
        try {
            const auto value = std::stoll(literal);
            if (value > std::numeric_limits<int>::max()) fail("register index overflow");
            return static_cast<int>(value);
        } catch (const std::exception&) {
            fail("invalid register index");
        }
    }
    [[noreturn]] void fail(const std::string& message) const {
        throw std::invalid_argument("accelerator source rejected at byte "
                + std::to_string(current().offset) + ": " + message
                + " (found '" + current().text + "')");
    }
private:
    std::vector<Token> tokens_;
    std::size_t at_ = 0;
};

inline GpuProgram compile_gpu(std::string_view source, dop::Plan layout) {
    Cursor p(source);
    p.expect("gpu");
    p.expect("fnc");
    auto name = p.identifier();
    p.expect("(");
    p.expect("f32");
    auto param = p.identifier();
    p.expect(")");
    p.expect(":");
    p.expect("f32");
    p.expect("{");
    p.expect("return");
    p.expect(param);
    float scale = 1.0f, bias = 0.0f;
    bool mul = p.take("*");
    if (mul) scale = p.f32();
    const bool plus = p.take("+");
    const bool minus = plus ? false : p.take("-");
    if (plus || minus) {
        bias = p.f32();
        if (minus) bias = -bias;
    }
    p.expect(";");
    p.expect("}");
    p.expect("<eof>");
    const auto op = mul && (plus || minus) ? dop::Op::ScaleBias
            : mul ? dop::Op::Scale : dop::Op::Bias;
    return GpuProgram{std::move(name), dop::Kernel(layout, dop::Element::F32, op, scale, bias)};
}

inline QuantumProgram compile_quantum(std::string_view source) {
    Cursor p(source);
    p.expect("quantum");
    p.expect("fnc");
    auto name = p.identifier();
    p.expect("(");
    p.expect(")");
    p.expect(":");
    p.expect("Circuit");
    p.expect("<");
    int qubits = p.index();
    p.expect(",");
    int bits = p.index();
    p.expect(">");
    p.expect("{");
    quantum::Builder builder(qubits, bits);
    int count = 0;
    while (!p.check("}") && !p.check("<eof>")) {
        if (++count > 1024) p.fail("circuit gate budget exceeded");
        auto gate = p.identifier();
        p.expect("(");
        auto a = p.index();
        if (gate == "x") builder.x(a);
        else if (gate == "h") builder.h(a);
        else if (gate == "cx" || gate == "measure") {
            p.expect(",");
            auto b = p.index();
            if (gate == "cx") builder.cx(a, b);
            else builder.measure(a, b);
        } else p.fail("unsupported quantum gate or host effect '" + gate + "'");
        p.expect(")");
        p.expect(";");
    }
    p.expect("}");
    p.expect("<eof>");
    return QuantumProgram{std::move(name), builder.build()};
}

} // namespace oreslang::accel_source
