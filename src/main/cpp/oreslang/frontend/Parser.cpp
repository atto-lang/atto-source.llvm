#include "oreslang/frontend/Parser.hpp"
#include "oreslang/types/OwnershipChecker.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace oreslang {
namespace {

enum class Kind { Identifier, Integer, Symbol, End };
struct Token {
    Kind kind;
    std::string_view text;
    std::size_t offset;
};

bool digit(char ch) { return ch >= '0' && ch <= '9'; }
bool identifier_start(char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_';
}
bool identifier_part(char ch) { return identifier_start(ch) || digit(ch); }

bool reserved(std::string_view name) {
    // Keep these reserved even while their grammar is unsupported.
    static constexpr std::string_view words[] = {
        "define", "class", "module", "contract", "conforms", "namespace",
        "import", "from", "as", "extends", "implements", "try", "catch",
        "finally", "end", "fi", "if", "do", "else", "then", "new",
        "spawn", "stop", "done", "await", "rt", "async", "suspends", "suspend", "critical", "generator",
        "nlex", "trap", "actor", "isoactor", "def", "fnc", "routine",
        "for", "of", "loop", "block", "break", "continue", "yield",
        "super", "elseif", "elif", "switch", "match", "matches", "is",
        "default", "eq", "neq", "nb", "select", "readch", "writech",
        "cb", "shared", "untrusted", "type", "types", "typeof",
        "interface", "trait", "struct", "impl", "abstract", "void",
        "static", "pub", "private", "return", "defer", "val", "const",
        "let", "mut", "borrow", "self", "true", "false", "null", "obj", "arr",
        "gpu", "quantum", "over", "structural", "int"
    };
    return std::find(std::begin(words), std::end(words), name) != std::end(words);
}

class Lexer {
public:
    explicit Lexer(std::string_view source) : source_(source) {}

    Token next() {
        for (;;) {
            if (at_ == source_.size()) return {Kind::End, {}, at_};
            const unsigned char ch = static_cast<unsigned char>(source_[at_]);
            if (std::isspace(ch)) { ++at_; continue; }
            if (source_.substr(at_, 2) == "//") {
                const auto end = source_.find('\n', at_ + 2);
                at_ = end == std::string_view::npos ? source_.size() : end;
                continue;
            }
            if (source_.substr(at_, 2) == "/*") {
                const auto start = at_;
                at_ += 2;
                std::size_t depth = 1;
                while (depth != 0) {
                    if (at_ == source_.size()) {
                        throw std::runtime_error("unterminated block comment at byte "
                                                 + std::to_string(start));
                    }
                    if (source_.substr(at_, 2) == "/*") { at_ += 2; ++depth; }
                    else if (source_.substr(at_, 2) == "*/") { at_ += 2; --depth; }
                    else ++at_;
                }
                continue;
            }
            break;
        }
        const std::size_t start = at_;
        const char ch = source_[at_];
        if (identifier_start(ch)) {
            ++at_;
            while (at_ < source_.size() && identifier_part(source_[at_])) ++at_;
            return {Kind::Identifier, source_.substr(start, at_ - start), start};
        }
        if (digit(ch)) {
            ++at_;
            while (at_ < source_.size() && (digit(source_[at_]) || source_[at_] == '_')) {
                ++at_;
            }
            return {Kind::Integer, source_.substr(start, at_ - start), start};
        }
        if (std::string_view("(){},:;+-*=").find(ch) != std::string_view::npos) {
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

std::unique_ptr<Expression> make_expression(Expression::Kind kind,
                                             std::size_t offset) {
    auto result = std::make_unique<Expression>();
    result->kind = kind;
    result->offset = offset;
    return result;
}

class Parser {
public:
    explicit Parser(std::string_view source) : lexer_(source), token_(lexer_.next()) {}

    Program parse() {
        Program program;
        bool wrapped_module = false;
        if (accept("define")) {
            require("module");
            program.module = identifier();
            accept("as"); // Both 'define module M as' and 'define module M' occur in Java.
            wrapped_module = true;
        }
        while (token_.kind != Kind::End
                && !(wrapped_module && token_.text == "end")) {
            program.functions.push_back(function());
        }
        if (wrapped_module) require("end");
        if (token_.kind != Kind::End) fail("unexpected trailing source");
        if (program.functions.empty()) fail("expected at least one function");
        validate(program);
        return program;
    }

private:
    Function function() {
        Function value;
        value.offset = token_.offset;
        value.exported = accept("pub");
        if (token_.text == "gpu" || token_.text == "quantum") {
            fail("native accelerator lowering unsupported");
        }
        if (token_.text == "suspends" || token_.text == "suspend") {
            fail("native suspends declaration requires suspension-aware borrow checking and LLVM continuation lowering");
        }
        if (token_.text == "critical") {
            fail("native critical declaration requires compiler lowering and scheduler-owned logical critical tickets");
        }
        if (token_.text == "actor" || token_.text == "async" ||
            token_.text == "generator" || token_.text == "isoactor") {
            fail("native actor/async declaration requires actor frames, suspension-aware borrow checking and LLVM lowering");
        }
        if (!accept("fnc") && !accept("routine")) fail("expected fnc or routine");
        value.name = identifier();
        require("(");
        if (!accept(")")) {
            do {
                Parameter parameter;
                if (accept("borrow")) {
                    parameter.mode = accept("mut") ? Parameter::Mode::BorrowMut
                                                    : Parameter::Mode::Borrow;
                }
                require("int");
                parameter.name = identifier();
                value.parameters.push_back(std::move(parameter));
            } while (accept(","));
            require(")");
        }
        require(":");
        require("int");
        require("{");
        while (token_.text == "const" || token_.text == "val") {
            LocalBinding binding;
            binding.offset = token_.offset;
            advance();
            binding.name = identifier();
            require(":");
            require("int");
            require("=");
            binding.initializer = expression();
            require(";");
            value.locals.push_back(std::move(binding));
        }
        require("return");
        value.result = expression();
        require(";");
        require("}");
        return value;
    }

    std::unique_ptr<Expression> expression() { return additive(); }

    std::unique_ptr<Expression> additive() {
        auto left = product();
        while (token_.text == "+" || token_.text == "-") {
            const auto offset = token_.offset;
            const auto op = token_.text;
            advance();
            auto expr = make_expression(op == "+" ? Expression::Kind::Add
                                                 : Expression::Kind::Subtract, offset);
            expr->left = std::move(left);
            expr->right = product();
            left = std::move(expr);
        }
        return left;
    }

    std::unique_ptr<Expression> product() {
        auto left = unary();
        while (token_.text == "*") {
            const auto offset = token_.offset;
            advance();
            auto expr = make_expression(Expression::Kind::Multiply, offset);
            expr->left = std::move(left);
            expr->right = unary();
            left = std::move(expr);
        }
        return left;
    }

    std::unique_ptr<Expression> unary() {
        if (token_.text == "rt") {
            const auto offset = token_.offset;
            advance();
            const auto operation = token_.text;
            if (operation != "borrow" && operation != "copy" &&
                operation != "take" && operation != "share") {
                fail("unsupported native rt operation; suspension/ownership lowering is unavailable");
            }
            advance();
            const bool mutable_borrow = operation == "borrow" && accept("mut");
            auto result = make_expression(
                operation == "copy" ? Expression::Kind::Copy :
                operation == "take" ? Expression::Kind::Take :
                operation == "share" ? Expression::Kind::Share :
                mutable_borrow ? Expression::Kind::BorrowMut : Expression::Kind::Borrow,
                offset);
            if (accept("(")) {
                result->left = expression();
                require(")");
            } else {
                result->left = unary();
            }
            return result;
        }
        if (token_.text == "-") {
            const auto offset = token_.offset;
            advance();
            auto expr = make_expression(Expression::Kind::Negate, offset);
            expr->left = unary();
            return expr;
        }
        return primary();
    }

    std::unique_ptr<Expression> primary() {
        const auto offset = token_.offset;
        if (accept("(")) {
            auto inner = expression();
            require(")");
            return inner;
        }
        if (token_.kind == Kind::Integer) {
            auto expr = make_expression(Expression::Kind::Integer, offset);
            std::string digits(token_.text);
            if (digits.front() == '_' || digits.back() == '_'
                    || digits.find("__") != std::string::npos) {
                fail("malformed integer separator");
            }
            digits.erase(std::remove(digits.begin(), digits.end(), '_'), digits.end());
            const auto parsed = std::from_chars(digits.data(),
                                                 digits.data() + digits.size(),
                                                 expr->integer);
            if (parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size()) {
                fail("integer literal overflows signed 64-bit result");
            }
            advance();
            return expr;
        }
        if (token_.kind != Kind::Identifier || reserved(token_.text)) {
            fail("expected int expression");
        }
        auto expr = make_expression(Expression::Kind::Variable, offset);
        expr->name = identifier();
        if (accept("(")) {
            expr->kind = Expression::Kind::Call;
            if (!accept(")")) {
                do {
                    expr->arguments.push_back(expression());
                } while (accept(","));
                require(")");
            }
        }
        return expr;
    }

    bool accept(std::string_view text) {
        if (token_.text != text) return false;
        advance();
        return true;
    }

    void require(std::string_view text) {
        if (!accept(text)) fail("expected '" + std::string(text) + "'");
    }

    std::string identifier() {
        if (token_.kind != Kind::Identifier || reserved(token_.text)) {
            fail("expected non-keyword identifier");
        }
        std::string result(token_.text);
        advance();
        return result;
    }

    static void validate_expression(const Expression& expr,
                                    const std::unordered_set<std::string>& parameters,
                                    const std::unordered_map<std::string, std::size_t>& functions) {
        switch (expr.kind) {
            case Expression::Kind::Integer: return;
            case Expression::Kind::Variable:
                if (parameters.count(expr.name) == 0) {
                    throw std::runtime_error("unknown parameter '" + expr.name
                                             + "' at byte " + std::to_string(expr.offset));
                }
                return;
            case Expression::Kind::Call: {
                const auto found = functions.find(expr.name);
                if (found == functions.end() || found->second != expr.arguments.size()) {
                    throw std::runtime_error("unknown function or wrong call arity '"
                                             + expr.name + "' at byte "
                                             + std::to_string(expr.offset));
                }
                for (const auto& arg : expr.arguments) {
                    validate_expression(*arg, parameters, functions);
                }
                return;
            }
            case Expression::Kind::Negate:
            case Expression::Kind::Copy:
            case Expression::Kind::Take:
            case Expression::Kind::Share:
            case Expression::Kind::Borrow:
            case Expression::Kind::BorrowMut:
                validate_expression(*expr.left, parameters, functions);
                return;
            case Expression::Kind::Add:
            case Expression::Kind::Subtract:
            case Expression::Kind::Multiply:
                validate_expression(*expr.left, parameters, functions);
                validate_expression(*expr.right, parameters, functions);
                return;
        }
        throw std::runtime_error("internal invalid expression kind");
    }

    static void validate(const Program& program) {
        std::unordered_map<std::string, std::size_t> functions;
        for (const auto& fn : program.functions) {
            if (!functions.emplace(fn.name, fn.parameters.size()).second) {
                throw std::runtime_error("duplicate function '" + fn.name + "' at byte "
                                         + std::to_string(fn.offset));
            }
            std::unordered_set<std::string> params;
            for (const auto& param : fn.parameters) {
                if (!params.insert(param.name).second) {
                    throw std::runtime_error("duplicate parameter '" + param.name
                                             + "' at byte " + std::to_string(fn.offset));
                }
            }
        }
        for (const auto& fn : program.functions) {
            std::unordered_set<std::string> params;
            for (const auto& param : fn.parameters) params.insert(param.name);
            for (const auto& binding : fn.locals) {
                // Validate before publishing the name: reject self/forward reads.
                validate_expression(*binding.initializer, params, functions);
                if (!params.insert(binding.name).second) {
                    throw std::runtime_error("duplicate owned local '" + binding.name
                        + "' at byte " + std::to_string(binding.offset));
                }
            }
            validate_expression(*fn.result, params, functions);
        }
    }

    void advance() { token_ = lexer_.next(); }
    [[noreturn]] void fail(const std::string& reason) const {
        if (token_.text == "gpu" || token_.text == "quantum") {
            throw std::runtime_error("native gpu/quantum boundary unsupported: "
                    "validated effects, ownership, device lowering and explicit "
                    "backend admission required; no implicit CPU fallback at byte "
                    + std::to_string(token_.offset));
        }
        // Syntactic admission is a safety boundary: rejecting actor/channel
        // keywords also means no unverified borrowed reference can enter a
        // suspended/native execution frame or deferred callback.
        if (token_.text == "select" || token_.text == "readch" ||
            token_.text == "writech" || token_.text == "nb" ||
            token_.text == "do" || token_.text == "await" ||
            token_.text == "yield" || token_.text == "cooperate") {
            throw std::runtime_error(
                "native async/channel/select boundary unsupported: "
                "suspension-aware borrow checking and cancellation/actor lowering required"
                " at byte " + std::to_string(token_.offset));
        }
        throw std::runtime_error(reason + " at byte " + std::to_string(token_.offset));
    }

    Lexer lexer_;
    Token token_;
};

} // namespace

Program parse_program(std::string_view source) {
    Program program = Parser(source).parse();
    // LLVM lowering is prohibited unless the native subset has passed both
    // name/arity checking and flow-sensitive explicit ownership admission.
    validate_i64_ownership(program);
    return program;
}

} // namespace oreslang
