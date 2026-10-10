#include "oreslang/frontend/Parser.hpp"

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
        "spawn", "stop", "done", "await", "rt", "async", "generator",
        "nlex", "trap", "actor", "isoactor", "def", "fnc", "routine",
        "for", "of", "loop", "block", "break", "continue", "yield",
        "super", "elseif", "elif", "switch", "match", "matches", "is",
        "default", "eq", "neq", "nb", "select", "readch", "writech",
        "cb", "shared", "untrusted", "type", "types", "typeof",
        "interface", "trait", "struct", "impl", "abstract", "void",
        "static", "pub", "private", "return", "defer", "val", "const",
        "let", "mut", "self", "true", "false", "null", "obj", "arr",
        "int", "owned"
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
            if (accept("module")) {
                program.module = identifier();
                accept("as");
                wrapped_module = true;
            } else if (accept("actor")) {
                program.actors.push_back(actor());
            } else {
                fail("expected 'module' or 'actor'");
            }
        }
        while (token_.kind != Kind::End
                && !(wrapped_module && token_.text == "end")) {
            if (accept("define")) {
                require("actor");
                program.actors.push_back(actor());
            } else {
                program.functions.push_back(function());
            }
        }
        if (wrapped_module) require("end");
        if (token_.kind != Kind::End) fail("unexpected trailing source");
        if (program.functions.empty() && program.actors.empty())
            fail("expected a function or actor");
        validate(program);
        return program;
    }

private:
    Actor actor() {
        Actor value;
        value.offset = token_.offset;
        value.name = identifier();
        require("as");
        const bool braced = accept("{");
        // The first native actor subset deliberately admits only immutable
        // scalar fields and read-only methods; no async or user destructors.
        while (token_.text == "const" || token_.text == "val") {
            LocalBinding field;
            field.offset = token_.offset;
            advance();
            field.name = identifier();
            require(":");
            require("int");
            require("=");
            field.initializer = expression();
            require(";");
            value.fields.push_back(std::move(field));
        }
        if (value.fields.empty()) fail("actor requires at least one scalar field");
        while (token_.text == "fnc" || token_.text == "routine" || token_.text == "pub") {
            value.methods.push_back(function());
        }
        if (value.methods.empty()) fail("actor requires at least one read-only method");
        if (braced) require("}");
        else require("end");
        return value;
    }

    Function function() {
        Function value;
        value.offset = token_.offset;
        value.exported = accept("pub");
        if (!accept("fnc") && !accept("routine")) fail("expected fnc or routine");
        value.name = identifier();
        require("(");
        if (!accept(")")) {
            do {
                require("int");
                value.parameters.push_back(identifier());
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
            // 'owned int' is a heap-owned scalar, intentionally explicit.
            // It never decays into the ordinary alloca-backed int type.
            binding.heap_owned = accept("owned");
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
                if (!params.insert(param).second) {
                    throw std::runtime_error("duplicate parameter '" + param
                                             + "' at byte " + std::to_string(fn.offset));
                }
            }
        }
        for (const auto& fn : program.functions) {
            std::unordered_set<std::string> available(fn.parameters.begin(),
                                                      fn.parameters.end());
            for (const auto& local : fn.locals) {
                // The initializer is checked BEFORE publication: x=x+1 or
                // forward references cannot read uninitialized stack memory.
                validate_expression(*local.initializer, available, functions);
                if (!available.insert(local.name).second) {
                    throw std::runtime_error("duplicate owned local '" + local.name
                                             + "' at byte " + std::to_string(local.offset));
                }
            }
            validate_expression(*fn.result, available, functions);
        }

        std::unordered_set<std::string> actorNames;
        std::unordered_set<std::string> generatedSymbols;
        auto reserveGenerated = [&](const std::string& name) {
            if (functions.count(name) || !generatedSymbols.insert(name).second)
                throw std::runtime_error("conflicting generated actor symbol '" + name + "'");
        };
        for (const auto& actor : program.actors) {
            if (!actorNames.insert(actor.name).second
                || functions.count(actor.name) != 0) {
                throw std::runtime_error("duplicate actor symbol '" + actor.name + "'");
            }
            const std::string prefix = "oreslang_actor_" + actor.name + "_";
            reserveGenerated(prefix + "init");
            std::unordered_set<std::string> fields;
            // Deliberately restrict initializers to closed arithmetic:
            // initialization must not escape the actor turn or call FFI.
            const std::unordered_set<std::string> none;
            const std::unordered_map<std::string, std::size_t> noCalls;
            for (const auto& field : actor.fields) {
                validate_expression(*field.initializer, none, noCalls);
                if (!fields.insert(field.name).second)
                    throw std::runtime_error("duplicate actor field '" + field.name + "'");
            }
            std::unordered_set<std::string> methods;
            for (const auto& method : actor.methods) {
                if (!methods.insert(method.name).second)
                    throw std::runtime_error("duplicate actor method '" + method.name + "'");
                reserveGenerated(prefix + method.name);
                std::unordered_set<std::string> available = fields;
                for (const auto& param : method.parameters) {
                    if (!available.insert(param).second)
                        throw std::runtime_error("actor parameter shadows field '" + param + "'");
                }
                for (const auto& local : method.locals) {
                    validate_expression(*local.initializer, available, functions);
                    if (!available.insert(local.name).second)
                        throw std::runtime_error("actor local shadows field/parameter '" + local.name + "'");
                }
                validate_expression(*method.result, available, functions);
            }
        }
    }

    void advance() { token_ = lexer_.next(); }
    [[noreturn]] void fail(const std::string& reason) const {
        throw std::runtime_error(reason + " at byte " + std::to_string(token_.offset));
    }

    Lexer lexer_;
    Token token_;
};

} // namespace

Program parse_program(std::string_view source) {
    return Parser(source).parse();
}

} // namespace oreslang
