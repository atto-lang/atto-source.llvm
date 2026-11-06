// G2 experimental AttoLang SAXPY device-kernel compiler, without host launch lowering.
// No GPU provider, ABI authority, memory loans, fence, or CPU fallback is created here.
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
struct Token { std::string text; std::size_t at; };
[[noreturn]] void fail(const std::string& reason, std::size_t at) {
    throw std::runtime_error("GPU-KERNEL-G2-001: " + reason + " at byte " + std::to_string(at));
}
bool alpha(char ch) { return (ch >= 'a' && ch <= 'z') ||
    (ch >= 'A' && ch <= 'Z') || ch == '_'; }
bool digit(char ch) { return ch >= '0' && ch <= '9'; }
bool alnum(char ch) { return alpha(ch) || digit(ch); }
std::vector<Token> lex(std::string_view source) {
    if (source.size() > 65536) fail("source exceeds 64KiB limit", 0);
    std::vector<Token> tokens;
    for (std::size_t i = 0; i < source.size();) {
        unsigned char ch = static_cast<unsigned char>(source[i]);
        if (std::isspace(ch)) { ++i; continue; }
        if (source.substr(i, 2) == "//") {
            i = source.find('\n', i);
            if (i == std::string_view::npos) break;
            continue;
        }
        if (source.substr(i, 2) == "/*") {
            auto close = source.find("*/", i + 2);
            if (close == std::string_view::npos) fail("unterminated comment", i);
            i = close + 2;
            continue;
        }
        const auto start = i;
        if (alpha(source[i])) {
            while (i < source.size() && alnum(source[i])) ++i;
            if (i - start > 128) fail("identifier exceeds 128 bytes", start);
        } else if (digit(source[i])) {
            while (i < source.size() && digit(source[i])) ++i;
        } else if (source.substr(i, 2) == "=>" || source.substr(i, 2) == ">=") {
            i += 2;
        } else if (std::string_view("(){}[]<>,;.+=*").find(source[i]) != std::string_view::npos) {
            ++i;
        } else fail("unsupported device character", start);
        tokens.push_back({std::string(source.substr(start, i-start)), start});
        if (tokens.size() > 1024) fail("token budget exceeded", start);
    }
    tokens.push_back({"<end>", source.size()});
    return tokens;
}
class Parser {
    std::vector<Token> tokens_;
    std::size_t cursor_ = 0;
    std::string scale_, x_, y_, output_, n_, index_;
    const Token& peek() const { return tokens_[cursor_]; }
    void take(std::string_view expected) {
        if (peek().text != expected)
            fail("expected '" + std::string(expected) + "', got '" + peek().text + "'", peek().at);
        ++cursor_;
    }
    std::string name() {
        const auto& t = peek();
        if (t.text.empty() || !alpha(t.text[0]) || t.text == "<end>")
            fail("expected identifier", t.at);
        static constexpr std::string_view reserved[] = {
            "gpu", "fnc", "pub", "const", "mut", "return", "if", "await",
            "let", "void", "u64", "f32", "GpuArray", "actor", "critical",
            "suspends", "class", "struct", "rt", "new", "launch"
        };
        for (const auto word : reserved)
            if (t.text == word) fail("reserved device binding name", t.at);
        ++cursor_;
        return t.text;
    }
    void ensure_distinct() {
        const std::vector<std::string> values{scale_, x_, y_, output_, n_, index_};
        for (std::size_t i = 0; i < values.size(); ++i)
            for (std::size_t j = i + 1; j < values.size(); ++j)
                if (values[i] == values[j]) fail("device bindings must be distinct", tokens_.back().at);
    }
public:
    explicit Parser(std::string_view source) : tokens_(lex(source)) {}
    void parse() {
        if (peek().text == "pub") take("pub");
        take("gpu"); take("fnc"); take("saxpy"); take("(");
        take("f32"); scale_ = name(); take(",");
        take("GpuArray"); take("<"); take("f32"); take(">"); x_ = name(); take(",");
        take("GpuArray"); take("<"); take("f32"); take(">"); y_ = name(); take(",");
        take("GpuArray"); take("<"); take("f32"); take(">"); take("mut"); output_ = name(); take(",");
        take("u64"); n_ = name(); take(")"); take("=>"); take("void"); take("{");
        take("const"); take("u64"); index_ = name(); take("=");
        take("gpu"); take("."); take("global_id"); take("("); take("0"); take(")"); take(";");
        take("if"); take(index_); take(">="); take(n_); take("{");
        take("return"); take(";"); take("}");
        take(output_); take("["); take(index_); take("]"); take("=");
        take(scale_); take("*"); take(x_); take("["); take(index_); take("]");
        take("+"); take(y_); take("["); take(index_); take("]"); take(";");
        if (peek().text == "return") { take("return"); take(";"); }
        take("}"); take("<end>");
        ensure_distinct();
    }
};
std::string lower_opencl(std::string_view atto) {
    Parser(atto).parse();
    // The accepted AST is the fixed, bounded SAXPY typed profile. Parameter
    // and local renaming is checked by the parser, so this emission normalizes
    // binding names without copying untrusted identifiers into device code.
    return "#pragma OPENCL FP_CONTRACT OFF\n"
        "__kernel void attolang_saxpy_v1(\n"
        "    const float scale, __global const float* x,\n"
        "    __global const float* y, __global float* out, const ulong n) {\n"
        "    const ulong i = get_global_id(0);\n"
        "    if (i >= n) return;\n"
        "    out[i] = scale * x[i] + y[i];\n"
        "}\n";
}
}
int main(int argc, char** argv) {
    try {
        if (argc != 3) throw std::runtime_error("usage: atto-gpu-saxpy-kernelc input.atto output.cl");
        const std::filesystem::path input(argv[1]), output(argv[2]);
        if (input.extension() != ".atto" || output.extension() != ".cl")
            throw std::runtime_error("input must be .atto and output must be .cl");
        if (std::filesystem::exists(output))
            throw std::runtime_error("refusing to overwrite an existing device artifact");
        std::ifstream file(input, std::ios::binary);
        if (!file) throw std::runtime_error("cannot open .atto kernel source");
        std::string source((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        std::string device_code = lower_opencl(source); // All checks precede any output file.
        std::ofstream result(output, std::ios::binary | std::ios::out);
        if (!result) throw std::runtime_error("cannot create output OpenCL artifact");
        result << device_code;
        result.flush();
        if (!result) { std::filesystem::remove(output); throw std::runtime_error("failed writing OpenCL artifact"); }
        std::cout << "G2_SOURCE_TO_OPENCL_C_ONLY kernel=attolang_saxpy_v1 bytes=" << device_code.size() << "\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << "\n";
        return 1;
    }
}
