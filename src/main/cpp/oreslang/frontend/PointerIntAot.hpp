#pragma once

// Explicitly opt-in closed-world Pointer<int> native AOT M1. This is a typed,
// nonescaping, statically bounded stack-promotion slice. It is NOT general
// Pointer<T>, actor transfer, heap allocation, or a dynamic lifetime engine.
// Full parse, type/effect and initialization checks precede any LLVM emission.
#include <array>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

namespace oreslang {
namespace pointer_int_aot {

struct Token {
    std::string text;
    std::size_t offset = 0;
    bool number = false;
};

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

inline std::vector<Token> lex(std::string_view source) {
    std::vector<Token> tokens;
    std::size_t pos = 0;
    while (pos < source.size()) {
        const std::size_t start = pos;
        const char c = source[pos];
        if (c == ' ' || c == '\n' || c == '\t' || c == '\r') {
            ++pos; continue;
        }
        if (source.substr(pos, 2) == "//") {
            pos += 2;
            while (pos < source.size() && source[pos] != '\n') ++pos;
            continue;
        }
        if (source.substr(pos, 2) == "/*") {
            pos += 2;
            std::size_t depth = 1;
            while (depth && pos < source.size()) {
                if (source.substr(pos, 2) == "/*") {
                    if (++depth > 32) reject("comment nesting limit exceeded", start);
                    pos += 2;
                } else if (source.substr(pos, 2) == "*/") {
                    --depth; pos += 2;
                } else ++pos;
            }
            if (depth) reject("unterminated block comment", start);
            continue;
        }
        if (tokens.size() >= 2048) reject("token budget exceeded", start);
        if (ident_start(c)) {
            ++pos;
            while (pos < source.size() && ident_continue(source[pos])) ++pos;
            tokens.push_back({std::string(source.substr(start, pos - start)), start, false});
            continue;
        }
        if (c >= '0' && c <= '9') {
            ++pos;
            while (pos < source.size() && source[pos] >= '0' && source[pos] <= '9') ++pos;
            tokens.push_back({std::string(source.substr(start, pos - start)), start, true});
            continue;
        }
        if (std::string_view("(){};,:<>=.+-*").find(c) != std::string_view::npos) {
            tokens.push_back({std::string(1, c), pos++, false});
            continue;
        }
        reject("unsupported token (no address-of, dereference, FFI or dynamic escape)", start);
    }
    tokens.push_back({"<eof>", source.size(), false});
    return tokens;
}

struct Expr {
    enum class Kind { Literal, Variable, Read, Neg, Add, Subtract, Multiply };
    Kind kind = Kind::Literal;
    std::int64_t known = 0;
    std::size_t index = 0;
    std::string name;
    std::unique_ptr<Expr> lhs;
    std::unique_ptr<Expr> rhs;
};

struct Step {
    enum class Kind { Write, Scalar, Close };
    Kind kind = Kind::Close;
    std::string name;
    std::size_t index = 0;
    std::unique_ptr<Expr> value;
};

struct Plan {
    std::string owner;
    std::size_t count = 0;
    std::vector<Step> steps;
    std::unique_ptr<Expr> result;
};

class Parser {
public:
    explicit Parser(std::string_view source) : tokens_(lex(source)) {}

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

// All source constructs and unsafe effects are validated before this emits IR.
// The optimizer may promote alloca into registers, but the emitted LLVM IR
// contains real typed stack memory, loads and stores, not an interpreter.
inline std::string emit_verified_ir(const Plan& plan) {
    llvm::LLVMContext context;
    llvm::Module module("atto.pointer.int.aot.m1", context);
    llvm::IRBuilder<> builder(context);
    auto* i64 = llvm::Type::getInt64Ty(context);
    auto* i32 = llvm::Type::getInt32Ty(context);
    auto* array = llvm::ArrayType::get(i64, plan.count);
    auto* main_type = llvm::FunctionType::get(i32, false);
    auto* main_fn = llvm::Function::Create(
        main_type, llvm::GlobalValue::ExternalLinkage, "main", module);
    auto* entry = llvm::BasicBlock::Create(context, "entry", main_fn);
    builder.SetInsertPoint(entry);
    auto* allocation = builder.CreateAlloca(array, nullptr, "pointer.int.region");
    std::unordered_map<std::string, llvm::Value*> scalar_values;
    auto slot = [&](std::size_t index) -> llvm::Value* {
        return builder.CreateInBoundsGEP(array, allocation,
            {builder.getInt64(0), builder.getInt64(index)}, "pointer.int.slot");
    };
    std::function<llvm::Value*(const Expr&)> lower;
    lower = [&](const Expr& expr) -> llvm::Value* {
        switch (expr.kind) {
        case Expr::Kind::Literal:
            return builder.getInt64(expr.known);
        case Expr::Kind::Variable:
            return scalar_values.at(expr.name);
        case Expr::Kind::Read:
            return builder.CreateLoad(i64, slot(expr.index), "pointer.read_at");
        case Expr::Kind::Neg:
            return builder.CreateNeg(lower(*expr.lhs), "scalar.negate");
        case Expr::Kind::Add:
            return builder.CreateAdd(lower(*expr.lhs), lower(*expr.rhs), "scalar.add");
        case Expr::Kind::Subtract:
            return builder.CreateSub(lower(*expr.lhs), lower(*expr.rhs), "scalar.subtract");
        case Expr::Kind::Multiply:
            return builder.CreateMul(lower(*expr.lhs), lower(*expr.rhs), "scalar.multiply");
        }
        throw std::runtime_error("unreachable pointer expression node");
    };
    for (const auto& step : plan.steps) {
        switch (step.kind) {
        case Step::Kind::Write:
            builder.CreateStore(lower(*step.value), slot(step.index));
            break;
        case Step::Kind::Scalar:
            scalar_values.emplace(step.name, lower(*step.value));
            break;
        case Step::Kind::Close:
            // Stack-promotion drop: overwrite every slot using volatile stores
            // before return. The stack frame is then reclaimed by the ABI.
            // No pointer or address can escape this function.
            for (std::size_t i = 0; i < plan.count; ++i) {
                auto* wiped = builder.CreateStore(builder.getInt64(0), slot(i));
                wiped->setVolatile(true);
            }
            break;
        }
    }
    builder.CreateRet(builder.CreateTrunc(lower(*plan.result), i32, "native.exit"));
    std::string errors;
    llvm::raw_string_ostream diagnostics(errors);
    if (llvm::verifyModule(module, &diagnostics))
        throw std::runtime_error("Pointer<int> LLVM verification failure: " + diagnostics.str());
    std::string result;
    llvm::raw_string_ostream output(result);
    module.print(output, nullptr);
    output.flush();
    return result;
}

inline std::string compile(std::string_view source) {
    return emit_verified_ir(Parser(source).parse());
}

} // namespace pointer_int_aot
} // namespace oreslang
