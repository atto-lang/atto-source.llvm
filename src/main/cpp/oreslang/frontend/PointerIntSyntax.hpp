#pragma once
// The closed-world pointer grammar now consumes the *normal native lexer*
// tokens. No private source scanner may reinterpret comments, symbols or
// identifiers. These M2a HIR nodes are published only after ownership checks.
#include "oreslang/types/PointerIntHIR.hpp"
#include <array>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace oreslang::pointer_int_aot {

[[noreturn]] inline void reject(std::string_view message, std::size_t at) {
    throw std::runtime_error("Pointer<int> native AOT M1: " +
        std::string(message) + " at byte " + std::to_string(at));
}

inline bool ident_start(char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_';
}
inline bool ident_continue(char ch) {
    return ident_start(ch) || (ch >= '0' && ch <= '9');
}


class Parser {
public:
    explicit Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {
        if (tokens_.empty() || tokens_.back().text != "<eof>")
            throw std::runtime_error("Pointer<int> canonical lexer requires explicit EOF");
    }

    Plan parse() {
        Plan plan;
        need("pub"); need("fnc"); need("main"); need("("); need(")");
        need(":"); need("int"); need("{");
        need("let"); need("mut"); need("Pointer"); need("<"); need("int"); need(">");
        plan.owner = identifier();
        if (plan.owner == "Pointer" || plan.owner == "main" || plan.owner == "int")
            reject("reserved pointer owner", token().offset);
        owner_ = plan.owner;
        need("="); need("Pointer"); need("."); need("alloc"); need("<");
        need("int"); need(">"); need("(");
        plan.count = index_literal(1, 16);
        need(")"); need(";");
        count_ = plan.count;
        while (token().text != "return") {
            if (token().text == "<eof>") reject("missing return and close", token().offset);
            if (plan.steps.size() >= 128) reject("pointer operation budget exceeded", token().offset);
            Step step;
            if (take("val")) {
                step.kind = Step::Kind::Scalar;
                need("int");
                step.name = identifier();
                if (step.name == owner_ || step.name == "main" || step.name == "Pointer" ||
                        known_locals_.count(step.name))
                    reject("duplicate or reserved immutable scalar", token().offset);
                need("=");
                step.value = expression();
                need(";");
                known_locals_.emplace(step.name, step.value->known);
            } else {
                need(owner_);
                need(".");
                if (take("write_at")) {
                    if (!live_) reject("write after close", token().offset);
                    step.kind = Step::Kind::Write;
                    need("(");
                    step.index = index_literal(0, count_ - 1);
                    need(",");
                    step.value = expression();
                    need(")"); need(";");
                    initialized_[step.index] = true;
                    contents_[step.index] = step.value->known;
                } else if (take("close")) {
                    if (!live_) reject("double close", token().offset);
                    step.kind = Step::Kind::Close;
                    need("("); need(")"); need(";");
                    live_ = false;
                } else {
                    reject("only write_at(index,value) or close() are pointer effects", token().offset);
                }
            }
            plan.steps.push_back(std::move(step));
        }
        need("return");
        plan.result = expression();
        need(";");
        need("}");
        if (token().text != "<eof>") reject("multiple functions or trailing input unsupported", token().offset);
        if (live_) reject("pointer must close exactly once before return", token().offset);
        return plan;
    }

private:
    std::vector<Token> tokens_;
    std::size_t at_ = 0;
    std::string owner_;
    std::size_t count_ = 0;
    bool live_ = true;
    std::array<bool, 16> initialized_{};
    std::array<std::int64_t, 16> contents_{};
    std::unordered_map<std::string, std::int64_t> known_locals_;

    const Token& token() const { return tokens_.at(at_); }
    bool take(std::string_view text) {
        if (token().text != text) return false;
        ++at_; return true;
    }
    void need(std::string_view text) {
        if (!take(text)) reject("expected '" + std::string(text) + "'", token().offset);
    }
    std::string identifier() {
        if (token().number || token().text.empty() || !ident_start(token().text[0]))
            reject("expected a named immutable binding", token().offset);
        const std::string result = token().text;
        ++at_; return result;
    }
    std::uint64_t unsigned_literal() {
        if (!token().number) reject("index/capacity must be a nonnegative integer literal", token().offset);
        std::uint64_t result = 0;
        const auto& text = token().text;
        const auto p = std::from_chars(text.data(), text.data() + text.size(), result);
        if (p.ec != std::errc{} || p.ptr != text.data() + text.size())
            reject("integer literal overflow", token().offset);
        ++at_; return result;
    }
    std::size_t index_literal(std::size_t minimum, std::size_t maximum) {
        const std::size_t offset = token().offset;
        const auto number = unsigned_literal();
        if (number < minimum || number > maximum)
            reject("pointer index/capacity outside static allocation bounds", offset);
        return static_cast<std::size_t>(number);
    }
    std::unique_ptr<Expr> atom() {
        if (take("(")) {
            auto expr = expression();
            need(")");
            return expr;
        }
        if (token().number) {
            const std::size_t offset = token().offset;
            const auto n = unsigned_literal();
            if (n > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
                reject("signed int literal overflow", offset);
            auto expr = std::make_unique<Expr>();
            expr->kind = Expr::Kind::Literal;
            expr->known = static_cast<std::int64_t>(n);
            return expr;
        }
        const std::size_t offset = token().offset;
        const auto name = identifier();
        if (name == owner_) {
            if (!live_) reject("read after close or pointer escape", offset);
            if (!take(".")) reject("Pointer<int> owner cannot escape as a scalar or handle", offset);
            need("read_at"); need("(");
            const auto index = index_literal(0, count_ - 1);
            need(")");
            if (!initialized_[index]) reject("read of uninitialized pointer element", offset);
            auto expr = std::make_unique<Expr>();
            expr->kind = Expr::Kind::Read;
            expr->index = index;
            expr->known = contents_[index];
            return expr;
        }
        const auto it = known_locals_.find(name);
        if (it == known_locals_.end()) reject("unknown scalar or unsupported pointer escape", offset);
        auto expr = std::make_unique<Expr>();
        expr->kind = Expr::Kind::Variable;
        expr->name = name;
        expr->known = it->second;
        return expr;
    }
    std::unique_ptr<Expr> unary() {
        if (take("+")) return unary();
        if (take("-")) {
            const auto offset = token().offset;
            auto child = unary();
            if (child->known == std::numeric_limits<std::int64_t>::min())
                reject("signed integer negation overflow", offset);
            auto result = std::make_unique<Expr>();
            result->kind = Expr::Kind::Neg;
            result->known = -child->known;
            result->lhs = std::move(child);
            return result;
        }
        return atom();
    }
    std::unique_ptr<Expr> term() {
        auto left = unary();
        while (take("*")) left = combine(Expr::Kind::Multiply, std::move(left), unary());
        return left;
    }
    std::unique_ptr<Expr> expression() {
        auto left = term();
        while (token().text == "+" || token().text == "-") {
            const bool add = take("+");
            if (!add) need("-");
            left = combine(add ? Expr::Kind::Add : Expr::Kind::Subtract,
                           std::move(left), term());
        }
        return left;
    }
    std::unique_ptr<Expr> combine(Expr::Kind kind, std::unique_ptr<Expr> lhs,
                                  std::unique_ptr<Expr> rhs) {
        const __int128 x = lhs->known;
        const __int128 y = rhs->known;
        const __int128 value = kind == Expr::Kind::Add ? x + y :
            kind == Expr::Kind::Subtract ? x - y : x * y;
        if (value < std::numeric_limits<std::int64_t>::min() ||
                value > std::numeric_limits<std::int64_t>::max())
            reject("checked arithmetic overflow", token().offset);
        auto expr = std::make_unique<Expr>();
        expr->kind = kind;
        expr->known = static_cast<std::int64_t>(value);
        expr->lhs = std::move(lhs);
        expr->rhs = std::move(rhs);
        return expr;
    }
};


} // namespace oreslang::pointer_int_aot
