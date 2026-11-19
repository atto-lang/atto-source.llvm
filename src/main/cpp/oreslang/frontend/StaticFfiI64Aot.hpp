#pragma once

// Experimental, closed-world, statically linked FFI i64 native AOT *proof*.
// NOT a production ffi actor runtime: no spawn, supervisor, authenticated
// artifact loader, cancellation, callbacks, pointers, hot reload or real
// suspending actor RPC is provided. This compiler profile is an intentional
// compile-only bridge probe, never enabled by default.
// The former PointerIntAot.hpp standalone lexer was retired by canonical
// Pointer<int> HIR M2a.  This deliberately restricted FFI proof must not
// reintroduce that obsolete compiler path or depend on its implementation.
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace oreslang {
namespace static_ffi_i64_aot {


struct Token {
    std::string text;
    std::size_t offset = 0;
    bool number = false;
};

[[noreturn]] inline void lex_reject(std::string_view reason, std::size_t offset) {
    throw std::runtime_error("static FFI i64 native AOT proof: " +
        std::string(reason) + " at byte " + std::to_string(offset));
}
inline bool ident_start(char ch) {
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_';
}
inline bool ident_continue(char ch) {
    return ident_start(ch) || (ch >= '0' && ch <= '9');
}
// Closed-world grammar: no source paths, dynamic libraries, punctuation with
// side effects, pointer syntax, annotations, or additional FFI operators.
// Tokenization is bounded and independent of the legacy pointer compiler.
inline std::vector<Token> lex(std::string_view source) {
    if (source.size() > 4096u) lex_reject("source size limit", 4096u);
    std::vector<Token> tokens;
    std::size_t pos = 0;
    while (pos < source.size()) {
        const std::size_t start = pos;
        const char ch = source[pos];
        if (ch == ' ' || ch == '\n' || ch == '\t' || ch == '\r') {
            ++pos;
            continue;
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
                    if (++depth > 32u) lex_reject("comment nesting limit", start);
                    pos += 2;
                } else if (source.substr(pos, 2) == "*/") {
                    --depth;
                    pos += 2;
                } else {
                    ++pos;
                }
            }
            if (depth) lex_reject("unterminated block comment", start);
            continue;
        }
        if (tokens.size() >= 128u) lex_reject("token budget exceeded", start);
        if (ident_start(ch)) {
            ++pos;
            while (pos < source.size() && ident_continue(source[pos])) ++pos;
            tokens.push_back({std::string(source.substr(start, pos - start)), start, false});
        } else if (ch >= '0' && ch <= '9') {
            ++pos;
            while (pos < source.size() && source[pos] >= '0' && source[pos] <= '9') ++pos;
            tokens.push_back({std::string(source.substr(start, pos - start)), start, true});
        } else if (std::string_view("(){},:;.").find(ch) != std::string_view::npos) {
            tokens.push_back({std::string(1, ch), pos++, false});
        } else {
            lex_reject("unsupported token", start);
        }
    }
    tokens.push_back({"<eof>", source.size(), false});
    return tokens;
}

struct Call {
    std::int64_t lhs;
    std::int64_t rhs;
};

class Parser {
public:
    explicit Parser(std::string_view text)
        : tokens_(lex(text)) {
        if (text.size() > 4096u || tokens_.size() > 128u)
            fail("source/token size limit");
    }

    Call parse() {
        // A single fixed foreign actor declaration with an immutable linker
        // binding. The source cannot choose a library path, symbol or ABI.
        need("define"); need("ffi"); need("actor"); need("NativeMath"); need("as");
        need("extern"); need("suspends"); need("fnc"); need("add_i64"); need("(");
        need("int"); need("a"); need(","); need("int"); need("b"); need(")");
        need(":"); need("int"); need(";"); need("end");
        // Deliberately narrower than the language: immediate adapter proof
        // only, with no claims that an actor was spawned or await suspended.
        need("pub"); need("suspends"); need("fnc"); need("main"); need("(");
        need(")"); need(":"); need("int"); need("using"); need("ffi");
        need(","); need("suspend"); need("{"); need("return"); need("await");
        need("NativeMath"); need("."); need("add_i64"); need("(");
        const auto lhs = number();
        need(",");
        const auto rhs = number();
        need(")"); need(";"); need("}"); need("<eof>");
        // The proof returns through a process exit code, so all arguments
        // and their mathematical sum must be in the 0..255 acceptance range.
        // This intentionally avoids truncating a foreign result into main().
        if (lhs + rhs > 255u)
            fail("checked exit result exceeds 255");
        return {static_cast<std::int64_t>(lhs), static_cast<std::int64_t>(rhs)};
    }

private:
    const std::vector<Token> tokens_;
    std::size_t at_ = 0;
    [[noreturn]] void fail(std::string_view message) const {
        const auto where = at_ < tokens_.size() ? tokens_[at_].offset : 0u;
        throw std::runtime_error("static FFI i64 native AOT proof: " +
            std::string(message) + " at byte " + std::to_string(where));
    }
    void need(std::string_view expected) {
        if (at_ >= tokens_.size() || tokens_[at_].text != expected)
            fail("expected immutable foreign ABI token");
        ++at_;
    }
    std::uint64_t number() {
        if (at_ >= tokens_.size() || !tokens_[at_].number)
            fail("literal i64 argument required");
        const auto& text = tokens_[at_++].text;
        // Exactly decimal 0..255; parse in bounded steps without an ambient
        // C integer width, locale, sign/hex/octal or leading-zero ambiguity.
        if (text.size() > 3 || (text.size() > 1 && text[0] == '0'))
            fail("noncanonical or oversized i64 literal");
        std::uint64_t value = 0;
        for (char c : text) {
            if (c < '0' || c > '9') fail("invalid decimal integer");
            value = value * 10u + static_cast<unsigned>(c - '0');
        }
        if (value > 255u) fail("literal exceeds checked exit range");
        return value;
    }
};

inline std::string compile(std::string_view source) {
    const auto call = Parser(source).parse(); // No output before full admission.
    llvm::LLVMContext context;
    llvm::Module module("atto.static.ffi.i64.aot.proof", context);
    llvm::IRBuilder<> builder(context);
    auto* i64 = llvm::Type::getInt64Ty(context);
    auto* i32 = llvm::Type::getInt32Ty(context);
    // Exact fixed-width C signature; no source-chosen symbol aliases or raw
    // pointers. The test must link a separate compiled C adapter.
    auto* foreignType = llvm::FunctionType::get(i64, {i64, i64}, false);
    auto* foreign = llvm::Function::Create(foreignType,
        llvm::GlobalValue::ExternalLinkage, "atto_add_i64", module);
    auto* mainType = llvm::FunctionType::get(i32, false);
    auto* mainFunction = llvm::Function::Create(mainType,
        llvm::GlobalValue::ExternalLinkage, "main", module);
    auto* entry = llvm::BasicBlock::Create(context, "entry", mainFunction);
    auto* exitOk = llvm::BasicBlock::Create(context, "ffi.ok", mainFunction);
    auto* exitBad = llvm::BasicBlock::Create(context, "ffi.invalid", mainFunction);
    builder.SetInsertPoint(entry);
    auto* value = builder.CreateCall(foreign,
        {builder.getInt64(call.lhs), builder.getInt64(call.rhs)}, "ffi.result");
    // Native callee returns signed i64. Unexpected negative/oversize results
    // are never silently coerced into a successful process exit byte.
    auto* admitted = builder.CreateICmpULE(value, builder.getInt64(255),
        "ffi.checked.exit.range");
    builder.CreateCondBr(admitted, exitOk, exitBad);
    builder.SetInsertPoint(exitOk);
    builder.CreateRet(builder.CreateTrunc(value, i32, "ffi.process.result"));
    builder.SetInsertPoint(exitBad);
    builder.CreateRet(builder.getInt32(255));
    std::string errors;
    llvm::raw_string_ostream diagnostics(errors);
    if (llvm::verifyModule(module, &diagnostics))
        throw std::runtime_error("static foreign LLVM verification failure: " +
            diagnostics.str());
    std::string text;
    llvm::raw_string_ostream out(text);
    module.print(out, nullptr);
    out.flush();
    return text;
}

} // namespace static_ffi_i64_aot
} // namespace oreslang
