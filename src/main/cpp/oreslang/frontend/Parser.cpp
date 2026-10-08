#include "oreslang/frontend/Parser.hpp"

#include <charconv>
#include <cctype>
#include <stdexcept>
#include <string>
#include <string_view>

namespace oreslang {
namespace {

enum class Kind { Identifier, Integer, Symbol, End };
struct Token {
    Kind kind;
    std::string_view text;
    std::size_t offset;
};

class Lexer {
public:
    explicit Lexer(std::string_view source) : source_(source) {}

    Token next() {
        while (at_ < source_.size()) {
            const unsigned char ch = static_cast<unsigned char>(source_[at_]);
            if (std::isspace(ch)) {
                ++at_;
                continue;
            }
            if (source_.substr(at_, 2) == "//") {
                at_ = source_.find('\n', at_);
                if (at_ == std::string_view::npos) at_ = source_.size();
                continue;
            }
            break;
        }
        if (at_ >= source_.size()) return {Kind::End, {}, at_};
        const std::size_t start = at_;
        const unsigned char ch = static_cast<unsigned char>(source_[at_]);
        if (std::isalpha(ch) || ch == '_') {
            ++at_;
            while (at_ < source_.size()) {
                const unsigned char c = static_cast<unsigned char>(source_[at_]);
                if (!std::isalnum(c) && c != '_') break;
                ++at_;
            }
            return {Kind::Identifier, source_.substr(start, at_ - start), start};
        }
        if (std::isdigit(ch)) {
            ++at_;
            while (at_ < source_.size()
                    && std::isdigit(static_cast<unsigned char>(source_[at_]))) {
                ++at_;
            }
            return {Kind::Integer, source_.substr(start, at_ - start), start};
        }
        if (std::string_view("(){}:;").find(static_cast<char>(ch))
                != std::string_view::npos) {
            ++at_;
            return {Kind::Symbol, source_.substr(start, 1), start};
        }
        throw std::runtime_error("unsupported Oreslang token at byte "
                                 + std::to_string(start));
    }

private:
    std::string_view source_;
    std::size_t at_ = 0;
};

class Parser {
public:
    explicit Parser(std::string_view source) : lexer_(source), current_(lexer_.next()) {}

    ConstantFunction parse() {
        ConstantFunction function;
        bool module = false;
        if (accept("define")) {
            require("module");
            function.module = identifier();
            require("as");
            module = true;
        }
        function.exported = accept("pub");
        if (!accept("fnc") && !accept("routine")) {
            fail("expected fnc or routine");
        }
        function.name = identifier();
        require("(");
        require(")");
        require(":");
        require("int");
        require("{");
        require("return");

        if (current_.kind != Kind::Integer) fail("expected integer literal");
        const std::string_view literal = current_.text;
        const auto parsed = std::from_chars(literal.data(),
                                            literal.data() + literal.size(),
                                            function.value);
        if (parsed.ec != std::errc{} || parsed.ptr != literal.data() + literal.size()) {
            fail("integer literal overflows signed 64-bit result");
        }
        advance();
        require(";");
        require("}");
        if (module) require("end");
        if (current_.kind != Kind::End) fail("unexpected trailing source");
        return function;
    }

private:
    bool accept(std::string_view value) {
        if (current_.text != value) return false;
        advance();
        return true;
    }
    void require(std::string_view value) {
        if (!accept(value)) fail("expected '" + std::string(value) + "'");
    }
    std::string identifier() {
        if (current_.kind != Kind::Identifier) fail("expected identifier");
        std::string result(current_.text);
        advance();
        return result;
    }
    void advance() { current_ = lexer_.next(); }
    [[noreturn]] void fail(const std::string& reason) const {
        throw std::runtime_error(reason + " at byte "
                                 + std::to_string(current_.offset));
    }

    Lexer lexer_;
    Token current_;
};

} // namespace

ConstantFunction parse_constant_function(std::string_view source) {
    return Parser(source).parse();
}
} // namespace oreslang
