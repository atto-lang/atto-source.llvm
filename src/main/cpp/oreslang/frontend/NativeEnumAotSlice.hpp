#pragma once

// Experimental, closed-world native guest-code AOT subset. This parser never
// executes Oreslang; it produces a checked list of exact eos2 declarations
// and statically bound native call sites for LLVM IR code generation.
//
// Accepted: one nullary enum, one-argument enum/case overloads returning
// literal int, and main(): int with explicitly typed immutable enum consts
// followed by a sum of direct calls. All other syntax fails closed.
// This is NOT the full Oreslang parser/typechecker/import/ownership system.
#include "oreslang/types/EnumCaseOverloadPolicy.hpp"

#include <charconv>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace oreslang::enum_aot {

struct Case { std::string name; std::int32_t tag; };
struct Function {
    types::case_overloads::Signature signature;
    std::int64_t returned = 0;
};
struct Call {
    std::string exactSymbol; // Bound *before* LLVM IR emission.
    std::int32_t tag = 0;
    std::string staticallyProvenCase; // empty means widened enum nominal type
};
struct Plan {
    std::string enumId;
    std::vector<Case> cases;
    std::vector<Function> functions;
    std::vector<Call> returnCalls;
};

/**
 * Bound the first process-entrypoint slice to a reliable POSIX exit status.
 * Every admitted guest overload body is currently a literal i64 return, so
 * this performs exact checked addition without interpreting guest code or
 * changing which overload was statically selected.
 *
 * Later general int-valued main functions must return an i64 guest result
 * through a dedicated entry wrapper instead of silently truncating it.
 */
inline std::uint8_t checkedNativeExit(const Plan& plan) {
    std::unordered_map<std::string, std::int64_t> bodies;
    for (const auto& fn : plan.functions) {
        if (!bodies.emplace(fn.signature.symbolId(), fn.returned).second)
            throw std::invalid_argument("native enum AOT: duplicate compiled slot");
    }
    std::int64_t total = 0;
    for (const auto& call : plan.returnCalls) {
        auto found = bodies.find(call.exactSymbol);
        if (found == bodies.end())
            throw std::invalid_argument("native enum AOT: missing compiled call target");
        const std::int64_t number = found->second;
        if ((number > 0 && total > std::numeric_limits<std::int64_t>::max() - number)
            || (number < 0 && total < std::numeric_limits<std::int64_t>::min() - number)) {
            throw std::invalid_argument("native enum AOT: signed int sum overflow");
        }
        total += number;
    }
    if (total < 0 || total > 255)
        throw std::invalid_argument("native enum AOT: process exit result outside 0..255");
    return static_cast<std::uint8_t>(total);
}

/**
 * Source-scanner-matching identity gate shared by parsing and LLVM lowering.
 * Reject anything this opt-in ASCII lexer would never construct, including
 * reserved language words and injected nested qualified IDs.
 */
inline void validateLexicalName(const std::string& name) {
    if (name.empty() || name.size() > types::case_overloads::MaxIdBytes)
        throw std::invalid_argument("native enum AOT: invalid identifier size");
    const auto start = [](unsigned char ch) {
        return (ch >= 'A' && ch <= 'Z') ||
               (ch >= 'a' && ch <= 'z') || ch == '_';
    };
    if (!start(static_cast<unsigned char>(name.front())))
        throw std::invalid_argument("native enum AOT: invalid initial identifier");
    for (unsigned char ch : name) {
        if (!start(ch) && !(ch >= '0' && ch <= '9'))
            throw std::invalid_argument("native enum AOT: nonportable identifier");
    }
    static const std::unordered_set<std::string> reserved{
        "define", "enum", "as", "end", "fnc", "routine", "pub",
        "private", "return", "int", "bool", "void", "const", "val",
        "let", "mut", "class", "module", "import", "from", "actor",
        "async", "suspend", "suspends", "match", "switch", "new",
        "if", "else", "true", "false", "null", "rt", "self"
    };
    if (reserved.count(name))
        throw std::invalid_argument("native enum AOT: reserved identifier");
}

class Parser final {
public:
    explicit Parser(std::string_view source) : source_(source) { advance(); }

    Plan parse() {
        expect("define"); expect("enum");
        enumName_ = identifier();
        plan_.enumId = "local_unit.__root__." + enumName_;
        if (plan_.enumId.size() > types::case_overloads::MaxIdBytes)
            fail("nominal enum identity exceeds 512-byte AOT ABI limit");
        expect("as");
        std::int32_t last = 0;
        std::unordered_set<std::string> seen;
        for (;;) {
            if (plan_.cases.size() >= types::case_overloads::MaxCases)
                fail("enum case budget exceeded");
            std::string name = identifier();
            if (!seen.insert(name).second) fail("duplicate enum case");
            std::int32_t tag = 0;
            if (eat("=")) {
                tag = positiveTag();
            } else {
                if (last == std::numeric_limits<std::int32_t>::max())
                    fail("implicit enum tag overflow");
                tag = last + 1;
            }
            if (tag <= last) fail("enum tags must increase monotonically");
            last = tag;
            plan_.cases.push_back({std::move(name), tag});
            if (!eat(",")) break;
        }
        expect("end");
        while (!end()) parseFunction();
        if (!mainSeen_ || unresolved_.empty() || plan_.functions.empty())
            fail("native enum AOT requires main, specialized functions and calls");
        const types::case_overloads::Registry registry {
            {plan_.enumId, {plan_.enumId, caseNames(), true}}
        };
        std::vector<types::case_overloads::Signature> family;
        family.reserve(plan_.functions.size());
        for (const auto& function : plan_.functions) {
            family.push_back(function.signature);
        }
        types::case_overloads::checkFamily(family, registry);
        for (std::size_t i = 0; i < unresolved_.size(); ++i) {
            const auto& call = unresolved_[i];
            if (call.name != functionName_) fail("unresolved AOT callable name");
            const auto& valueType = call.valueType;
            const auto arg = valueType.second.empty()
                ? types::case_overloads::Parameter::whole(plan_.enumId)
                : types::case_overloads::Parameter::selectedCase(
                      plan_.enumId, valueType.second);
            const std::size_t selected =
                types::case_overloads::selectStatic(family, registry, {arg});
            plan_.returnCalls.push_back({
                family.at(selected).symbolId(), call.tag, valueType.second
            });
        }
        (void)checkedNativeExit(plan_); // Do not emit partial IR for overflowing input.
        return std::move(plan_);
    }

private:
    struct Token { std::string text; std::size_t offset = 0; };
    struct Pending {
        std::string name;
        std::pair<std::string, std::string> valueType;
        std::int32_t tag;
    };
    struct Variable {
        std::string declaredCase; // empty means widened nominal enum
        std::int32_t tag;
    };

    void fail(const char* reason) const {
        throw std::invalid_argument(std::string("native enum AOT: ") + reason
                                    + " at byte " + std::to_string(token_.offset));
    }
    bool end() const { return token_.text.empty(); }
    void advance() {
        while (pos_ < source_.size()) {
            const unsigned char c = static_cast<unsigned char>(source_[pos_]);
            if (c == ' ' || c == '\n' || c == '\r' || c == '\t') { ++pos_; continue; }
            if (source_.substr(pos_, 2) == "//") {
                auto to = source_.find('\n', pos_ + 2);
                pos_ = to == std::string_view::npos ? source_.size() : to;
                continue;
            }
            break;
        }
        token_ = {{}, pos_};
        if (pos_ >= source_.size()) return;
        const unsigned char first = static_cast<unsigned char>(source_[pos_]);
        const auto letter = [](unsigned char c) {
            return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
        };
        if (letter(first)) {
            auto start = pos_++;
            while (pos_ < source_.size()) {
                unsigned char c = static_cast<unsigned char>(source_[pos_]);
                if (!letter(c) && !(c >= '0' && c <= '9')) break;
                ++pos_;
            }
            token_.text = std::string(source_.substr(start, pos_ - start));
        } else if (first >= '0' && first <= '9') {
            auto start = pos_++;
            while (pos_ < source_.size()) {
                unsigned char c = static_cast<unsigned char>(source_[pos_]);
                if (c < '0' || c > '9') break;
                ++pos_;
            }
            token_.text = std::string(source_.substr(start, pos_ - start));
        } else {
            if (std::string_view("(){}.,;:=+-").find(first) == std::string_view::npos)
                fail("unsupported character or comment in static AOT subset");
            token_.text.assign(1, static_cast<char>(first));
            ++pos_;
        }
        if (token_.text.size() > types::case_overloads::MaxIdBytes)
            fail("token byte budget exceeded");
    }
    bool eat(std::string_view what) {
        if (token_.text != what) return false;
        advance();
        return true;
    }
    void expect(std::string_view what) {
        if (!eat(what)) fail("unexpected token");
    }
    std::string identifier() {
        const std::string text = token_.text;
        validateLexicalName(text);
        advance();
        return text;
    }
    std::int64_t literalInt() {
        bool negative = false;
        if (eat("-")) negative = true;
        if (token_.text.empty() || token_.text[0] < '0' || token_.text[0] > '9')
            fail("integer literal required");
        // Parse the *magnitude* as unsigned. Parsing into int64_t first
        // incorrectly rejects the valid -9223372036854775808 literal, and
        // negating INT64_MIN after parsing would be undefined behavior.
        std::uint64_t magnitude = 0;
        auto parsed = std::from_chars(token_.text.data(),
                                      token_.text.data()+token_.text.size(), magnitude);
        const std::uint64_t maximum =
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
        if (parsed.ec != std::errc{} ||
            parsed.ptr != token_.text.data()+token_.text.size() ||
            magnitude > maximum + (negative ? 1u : 0u))
            fail("invalid or out-of-range integer literal");
        advance();
        if (negative && magnitude == maximum + 1u)
            return std::numeric_limits<std::int64_t>::min();
        const std::int64_t value = static_cast<std::int64_t>(magnitude);
        return negative ? -value : value;
    }
    std::int32_t positiveTag() {
        std::int64_t value = literalInt();
        if (value <= 0 || value > std::numeric_limits<std::int32_t>::max())
            fail("enum tag outside positive int32 domain");
        return static_cast<std::int32_t>(value);
    }
    std::pair<std::string, std::string> typeRef() {
        std::string e = identifier();
        if (e != enumName_) fail("only locally declared enum type is supported");
        std::string variant;
        if (eat(".")) {
            variant = identifier();
            (void)tagOf(variant);
        }
        return {e, variant};
    }
    std::int32_t tagOf(const std::string& caseName) const {
        for (const auto& item : plan_.cases)
            if (item.name == caseName) return item.tag;
        fail("unresolved enum case");
        return 0;
    }
    std::pair<std::string, std::int32_t> caseLiteral() {
        std::string en = identifier();
        if (en != enumName_) fail("unlinked enum constructor");
        expect(".");
        std::string caseName = identifier();
        return {caseName, tagOf(caseName)};
    }
    std::vector<std::string> caseNames() const {
        std::vector<std::string> result;
        result.reserve(plan_.cases.size());
        for (const auto& item : plan_.cases) result.push_back(item.name);
        return result;
    }
    void parseFunction() {
        eat("pub");
        expect("fnc");
        const std::string name = identifier();
        expect("(");
        if (name == "main") {
            if (mainSeen_) fail("duplicate native enum AOT main");
            mainSeen_ = true;
            expect(")");
            expect(":"); expect("int"); expect("{");
            parseMainBody();
            expect("}");
            return;
        }
        if (mainSeen_) fail("AOT function declarations must precede main");
        auto type = typeRef();
        (void)identifier(); // parameter name has no effect on a literal-only body.
        expect(")"); expect(":"); expect("int"); expect("{");
        expect("return");
        std::int64_t value = literalInt();
        expect(";"); expect("}");
        if (plan_.functions.size() >= types::case_overloads::MaxCandidates)
            fail("overload candidate budget exceeded");
        if (!functionName_.empty() && functionName_ != name)
            fail("only one overload family admitted by native AOT slice");
        functionName_ = name;
        const auto parameter = type.second.empty()
            ? types::case_overloads::Parameter::whole(plan_.enumId)
            : types::case_overloads::Parameter::selectedCase(plan_.enumId, type.second);
        plan_.functions.push_back({
            {types::case_overloads::Domain::Module,
             "local_unit.__root__", name, {parameter}},
            value
        });
    }
    void parseMainBody() {
        std::unordered_map<std::string, Variable> locals;
        while (eat("const")) {
            auto declared = typeRef();
            const std::string name = identifier();
            if (locals.count(name) || name == enumName_ || name == functionName_)
                fail("duplicate or shadowing local");
            expect("=");
            auto literal = caseLiteral();
            expect(";");
            if (!declared.second.empty() && declared.second != literal.first)
                fail("enum case cannot be assigned to unrelated case refinement");
            if (locals.size() >= 64) fail("too many AOT locals");
            locals.emplace(name, Variable{declared.second, literal.second});
        }
        expect("return");
        for (;;) {
            const std::string callee = identifier();
            expect("(");
            Pending call;
            call.name = callee;
            if (token_.text == enumName_) {
                auto literal = caseLiteral();
                call.valueType = {enumName_, literal.first};
                call.tag = literal.second;
            } else {
                const auto name = identifier();
                auto found = locals.find(name);
                if (found == locals.end()) fail("unresolved or mutable call argument");
                call.valueType = {enumName_, found->second.declaredCase};
                call.tag = found->second.tag;
            }
            expect(")");
            if (unresolved_.size() >= 64) fail("too many native enum AOT calls");
            unresolved_.push_back(std::move(call));
            if (!eat("+")) break;
        }
        expect(";");
    }

    std::string_view source_;
    std::size_t pos_ = 0;
    Token token_;
    std::string enumName_;
    std::string functionName_;
    bool mainSeen_ = false;
    Plan plan_;
    std::vector<Pending> unresolved_;
};

inline Plan parse(std::string_view text) {
    try {
        return Parser(text).parse();
    } catch (const std::invalid_argument& error) {
        const std::string message(error.what());
        if (message.rfind("native enum AOT:", 0) == 0) throw;
        throw std::invalid_argument("native enum AOT: " + message);
    }
}

} // namespace oreslang::enum_aot
