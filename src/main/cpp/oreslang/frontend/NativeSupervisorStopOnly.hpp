#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oreslang {

/**
 * A separate, opt-in and strictly bounded compiler for the first Java-parity
 * supervisor source hook. No general LLVM guest code is emitted here; the
 * plan is interpreted only by the trusted native CONTROL owner. The regular
 * parse_program() path deliberately continues to reject supervisors.
 */
class NativeSupervisorStopOnlyPlan final {
public:
    enum class Target { CurrentFailure };

    const std::string& name() const noexcept { return name_; }
    Target target() const noexcept { return target_; }
    bool explicitly_bound_failure() const noexcept { return explicit_failure_; }

private:
    friend NativeSupervisorStopOnlyPlan compile_native_stop_only_supervisor(std::string_view);
    NativeSupervisorStopOnlyPlan(std::string name, Target target, bool explicit_failure)
        : name_(std::move(name)), target_(target), explicit_failure_(explicit_failure) {}
    std::string name_;
    Target target_;
    bool explicit_failure_;
};

namespace supervisor_stop_source_detail {

[[noreturn]] inline void denied(std::string_view reason) {
    throw std::invalid_argument("NATIVE_SUPERVISOR_STOP_ONLY_DENIED: " + std::string(reason));
}
inline bool alpha(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
inline bool digit(char c) noexcept { return c >= '0' && c <= '9'; }
inline bool part(char c) noexcept { return alpha(c) || digit(c); }
inline bool space(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}
struct Token {
    std::string_view value;
    bool identifier;
};

inline std::vector<Token> lex(std::string_view source) {
    if (source.size() > 1'048'576) denied("source byte budget exceeded");
    if (source.find('\0') != std::string_view::npos)
        denied("NUL is forbidden even inside comments");
    std::vector<Token> result;
    result.reserve(48);
    for (std::size_t i = 0; i < source.size();) {
        const char c = source[i];
        if (space(c)) { ++i; continue; }
        if (i + 1 < source.size() && source.substr(i, 2) == "//") {
            const auto eol = source.find('\n', i + 2);
            i = eol == std::string_view::npos ? source.size() : eol + 1;
            continue;
        }
        if (i + 1 < source.size() && source.substr(i, 2) == "/*") {
            i += 2;
            unsigned depth = 1;
            while (depth != 0 && i < source.size()) {
                if (source.substr(i, 2) == "/*") {
                    if (++depth > 32) denied("nested comment budget exceeded");
                    i += 2;
                } else if (source.substr(i, 2) == "*/") {
                    --depth;
                    i += 2;
                } else {
                    ++i;
                }
            }
            if (depth != 0) denied("unterminated comment");
            continue;
        }
        if (result.size() >= 96) denied("token budget exceeded");
        if (alpha(c)) {
            const auto start = i++;
            while (i < source.size() && part(source[i])) ++i;
            if (i - start > 128) denied("identifier budget exceeded");
            result.push_back({source.substr(start, i - start), true});
        } else if (source.substr(i, 2) == "->") {
            result.push_back({source.substr(i, 2), false});
            i += 2;
        } else if (std::string_view("{}().:;@").find(c) != std::string_view::npos) {
            result.push_back({source.substr(i, 1), false});
            ++i;
        } else {
            // Includes quotes, escaped macros, arbitrary operators and
            // non-ASCII confusables. Do not strip unsupported source text.
            denied("unsupported token in restricted supervisor source");
        }
    }
    return result;
}

class CheckedTokens final {
public:
    explicit CheckedTokens(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}
    bool accept(std::string_view value) {
        if (pos_ < tokens_.size() && tokens_[pos_].value == value) {
            ++pos_;
            return true;
        }
        return false;
    }
    void require(std::string_view value) {
        if (!accept(value)) denied("unexpected token or unsupported supervisor effect");
    }
    std::string identifier() {
        if (pos_ == tokens_.size() || !tokens_[pos_].identifier)
            denied("expected checked identifier");
        const auto value = tokens_[pos_++].value;
        // Match the ordinary frontend's reserved-identifier boundary even
        // though this intentionally separate source slice has no general AST.
        for (const auto banned : {
            "self", "return", "end", "as", "define",
            "declare", "pub", "private", "await", "async",
            "void", "Failure", "on_panic", "stop", "child",
            "actor", "isoactor", "supervisor", "class", "module",
            "namespace", "import", "from", "trait", "interface",
            "enum", "type", "struct", "fnc", "routine",
            "method", "mtd", "static", "gen", "generator",
            "int", "bool", "true", "false", "null",
            "let", "val", "final", "const", "mut",
            "while", "for", "if", "then", "else",
            "fi", "match", "switch", "select", "throw",
            "raise", "panic", "trap", "defer", "recover",
            "try", "catch", "finally", "extends", "implements",
            "conforms", "with", "using", "pattern", "contract",
            "do", "new", "spawn", "done", "rt",
            "suspends", "suspend", "critical", "nlex", "def",
            "of", "loop", "block", "break", "continue",
            "yield", "super", "elseif", "elif", "matches",
            "is", "default", "eq", "neq", "nb",
            "readch", "writech", "cb", "shared", "untrusted",
            "types", "typeof", "impl", "abstract", "Iterator",
            "AsyncIterator", "borrow", "obj", "arr", "gpu",
            "cpu", "quantum", "over", "structural", "pure",
            "Some", "None", "Ok", "Err", "requires",
            "serial", "critical-read", "critical-write", "nb-select"
        }) {
            if (value == banned) denied("reserved supervisor identifier");
        }
        return std::string(value);
    }
    bool finished() const noexcept { return pos_ == tokens_.size(); }
private:
    std::vector<Token> tokens_;
    std::size_t pos_ = 0;
};

} // namespace supervisor_stop_source_detail

inline NativeSupervisorStopOnlyPlan compile_native_stop_only_supervisor(std::string_view source) {
    using namespace supervisor_stop_source_detail;
    CheckedTokens t(lex(source));
    const bool pub_before = t.accept("pub");
    t.require("define");
    const bool pub_after = t.accept("pub");
    if (pub_before && pub_after) denied("duplicate pub modifier");
    t.require("supervisor");
    const std::string name = t.identifier();
    t.require("as");
    if (t.accept("@")) t.require("Implements");
    t.accept("pub");
    // A real await needs a pinned CONTROL continuation and a CompletionStage
    // receipt. Refuse both async modifier and await, never ignore suspension.
    if (t.accept("async")) denied("async STOP continuation not lowered");
    t.require("on_panic");
    t.require("(");
    const std::string binder = t.identifier();
    t.require(":");
    t.require("Failure");
    t.require(")");
    if (!t.accept(":")) t.require("->");
    t.require("void");
    t.require("{");
    t.require("self");
    t.require(".");
    t.require("stop");
    t.require("(");
    bool explicit_failure = false;
    auto target = NativeSupervisorStopOnlyPlan::Target::CurrentFailure;
    if (!t.accept(")")) {
        t.require(binder);
        explicit_failure = true;
        if (t.accept(".")) {
            t.require("child");
            denied("logical child target needs checked native ChildId binding");
        }
        t.require(")");
    }
    t.require(";");
    t.require("return");
    t.require(";");
    t.require("}");
    t.require("end");
    if (!t.finished()) denied("extra declarations or effects after supervisor hook");
    return NativeSupervisorStopOnlyPlan(name, target, explicit_failure);
}

} // namespace oreslang
