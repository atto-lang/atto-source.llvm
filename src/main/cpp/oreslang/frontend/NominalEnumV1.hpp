#pragma once
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace oreslang::frontend {

// Source lowering is deliberately separate from general source admission.
// Only a sealed, locally constructed, nullary enum with a complete match can
// be encoded in the existing checked native i64 CFG. V2 also checks a single
// internal static helper with nominal argument and return types at source
// level. No payload, mutable symbol, actor mail, public enum parameter ABI,
// numeric guest tag, imported function, or raw-tag caller exists.
inline std::string lower_nominal_enum_v1(std::string_view source) {
    struct Token { std::string value; std::size_t offset = 0; bool identifier = false; };
    auto alpha = [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    };
    auto digit = [](unsigned char c) { return c >= '0' && c <= '9'; };
    if (source.size() > 1024 * 1024)
        throw std::runtime_error("ADT v1 source exceeds 1MiB budget");
    std::vector<Token> tokens;
    for (std::size_t i = 0; i < source.size();) {
        const unsigned char c = static_cast<unsigned char>(source[i]);
        if (c == 0)
            throw std::runtime_error("ADT v1 source contains NUL");
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f') {
            ++i; continue;
        }
        if (c == '/' && i + 1 < source.size() && source[i + 1] == '/') {
            i += 2;
            while (i < source.size() && source[i] != '\n' && source[i] != '\r')
                ++i;
            continue;
        }
        if (c == '/' && i + 1 < source.size() && source[i + 1] == '*') {
            const std::size_t at = i;
            i += 2;
            unsigned depth = 1;
            while (depth != 0) {
                if (i >= source.size())
                    throw std::runtime_error("ADT v1 unterminated block comment at byte " +
                        std::to_string(at));
                if (i + 1 < source.size() && source[i] == '/' && source[i + 1] == '*') {
                    if (++depth > 128)
                        throw std::runtime_error("ADT v1 comment nesting exceeds limit");
                    i += 2;
                } else if (i + 1 < source.size() && source[i] == '*' && source[i + 1] == '/') {
                    --depth;
                    i += 2;
                } else ++i;
            }
            continue;
        }
        const std::size_t at = i;
        bool is_ident = false;
        if (alpha(c)) {
            is_ident = true;
            ++i;
            while (i < source.size() &&
                    (alpha(static_cast<unsigned char>(source[i])) ||
                     digit(static_cast<unsigned char>(source[i]))))
                ++i;
            if (i - at > 128)
                throw std::runtime_error("ADT v1 identifier exceeds 128 bytes");
        } else if (digit(c)) {
            ++i;
            while (i < source.size() && digit(static_cast<unsigned char>(source[i])))
                ++i;
            if (i - at > 19)
                throw std::runtime_error("ADT v1 numeric token exceeds i64 budget");
        } else if (c == '-' && i + 1 < source.size() && source[i + 1] == '>') {
            i += 2;
        } else if (c == '(' || c == ')' || c == '{' || c == '}' ||
                   c == '.' || c == ':' || c == ';' || c == '=' || c == ',') ++i;
        else throw std::runtime_error("ADT v1 unsupported source token at byte " +
            std::to_string(at));
        if (tokens.size() >= 10000)
            throw std::runtime_error("ADT v1 token count exceeds limit");
        tokens.push_back({std::string(source.substr(at, i - at)), at, is_ident});
    }
    tokens.push_back({"", source.size(), false});
    struct Reader {
        std::vector<Token> tokens;
        std::size_t i = 0;
        const Token& current() const { return tokens.at(std::min(i, tokens.size() - 1)); }
        bool take(std::string_view expected) {
            if (current().value != expected) return false;
            if (i < tokens.size() - 1) ++i;
            return true;
        }
        void need(std::string_view expected) {
            if (!take(expected))
                throw std::runtime_error("ADT v1 expected '" + std::string(expected) +
                    "' at byte " + std::to_string(current().offset));
        }
        std::string name() {
            static constexpr std::string_view keywords[] = {
                "int", "bool", "define", "enum", "end", "match", "over", "on",
                "return", "pub", "fnc", "val", "let", "main", "when", "if", "as", "do",
                "then", "else", "fi", "true", "false"
            };
            if (!current().identifier)
                throw std::runtime_error("ADT v1 expected unreserved identifier");
            for (const auto keyword : keywords)
                if (current().value == keyword)
                    throw std::runtime_error("ADT v1 reserved symbol identifier");
            return std::exchange(tokens.at(i++).value, {});
        }
        std::int64_t number() {
            const auto& s = current().value;
            std::int64_t out = 0;
            const auto [ptr, ec] = std::from_chars(
                s.data(), s.data() + s.size(), out);
            if (s.empty() || ec != std::errc() || ptr != s.data() + s.size() ||
                    out > std::numeric_limits<std::int32_t>::max())
                throw std::runtime_error("ADT v1 integer out of signed i32 range");
            if (i < tokens.size() - 1) ++i;
            return out;
        }
        std::pair<std::string, std::string> qualified() {
            auto owner = name();
            need(".");
            auto which = name();
            if (take("(")) need(")");
            return {std::move(owner), std::move(which)};
        }
        // A pattern may omit its enum owner only when the scrutinee owns
        // the complete closed nominal schema. Trailing `as alias` is scoped
        // to the selected arm and never creates a guest runtime allocation.
        std::pair<std::string, std::string> nominal_pattern(
                std::string_view owner, std::string_view foreign_error) {
            const auto first = name();
            std::string variant = first;
            if (take(".")) {
                if (first != owner)
                    throw std::runtime_error(std::string(foreign_error));
                variant = name();
            }
            if (current().value == "{") {
                throw std::runtime_error(
                    "ADT v3 named enum payload patterns require typed payload AOT lowering");
            }
            std::string alias;
            if (take("as")) {
                alias = name();
                if (alias == owner || alias == variant)
                    throw std::runtime_error("ADT v3 alias shadows nominal declaration or variant");
            }
            return {std::move(variant), std::move(alias)};
        }
    };
    Reader r{std::move(tokens)};
    r.need("define"); r.need("enum");
    const auto owner = r.name();
    // Legacy `define enum E as ... end` remains admitted in this opt-in
    // compiler slice; canonical closed nullary syntax uses braces and commas.
    const bool brace_enum = r.take("{");
    if (!brace_enum) r.need("as");
    struct Variant { std::string name; std::int64_t tag = 0; };
    std::vector<Variant> variants;
    std::unordered_set<std::string> used_names;
    std::unordered_set<std::int64_t> used_tags;
    std::int64_t next_tag = 1;
    while (r.current().value != (brace_enum ? "}" : "end")) {
        if (variants.size() == 32 || r.current().value.empty())
            throw std::runtime_error("ADT v1 enum case count exceeds bounds");
        const auto name = r.name();
        if (r.current().value == "{") {
            throw std::runtime_error(
                "ADT v3 payload-bearing enums require checked payload AOT lowering");
        }
        if (!used_names.insert(name).second)
            throw std::runtime_error("ADT v1 duplicate nominal case");
        const auto tag = r.take("=") ? r.number() : next_tag;
        if (tag <= 0 || tag > std::numeric_limits<std::int32_t>::max() ||
                !used_tags.insert(tag).second)
            throw std::runtime_error("ADT v1 duplicate/invalid case tag");
        variants.push_back({name, tag});
        next_tag = tag + 1;
        if (brace_enum) {
            if (!r.take(",") && r.current().value != "}")
                throw std::runtime_error("ADT v3 expected comma between enum variants");
        } else r.take(";");
    }
    r.need(brace_enum ? "}" : "end"); r.take(";");
    if (variants.size() < 2)
        throw std::runtime_error("ADT v1 requires at least two nullary cases");
    // Parse/check source-nominal helper signature and every return BEFORE
    // lowering it to an internal private static scalar call. A foreign
    // enum owner can never enter that helper as an untyped i64 argument.
    std::string helper_name, helper_lowered;
    if (r.take("fnc")) {
        helper_name = r.name();
        if (helper_name == owner ||
                std::any_of(variants.begin(), variants.end(),
                    [&](const Variant& v) { return v.name == helper_name; }))
            throw std::runtime_error("ADT v2 helper conflicts with nominal symbol");
        r.need("("); r.need(owner);
        const auto param = r.name();
        if (param == owner || param == helper_name)
            throw std::runtime_error("ADT v2 helper parameter shadows nominal symbol");
        r.need(")"); r.need(":"); r.need(owner); r.need("{");
        r.take("do"); r.need("match"); r.need(param); r.need("over");
        struct TypedArm { std::int64_t tag = 0, returned = 0; };
        std::vector<TypedArm> helper_arms;
        std::unordered_set<std::string> helper_seen;
        while (r.take("on")) {
            const auto pattern = r.nominal_pattern(
                owner, "ADT v2 helper pattern foreign nominal owner");
            if (!pattern.second.empty() &&
                    (pattern.second == param || pattern.second == helper_name ||
                    std::any_of(variants.begin(), variants.end(),
                        [&](const Variant& v) { return v.name == pattern.second; })))
                throw std::runtime_error("ADT v3 helper alias shadows an existing nominal binding");
            const auto which = std::find_if(variants.begin(), variants.end(),
                [&](const Variant& v) { return v.name == pattern.first; });
            if (which == variants.end())
                throw std::runtime_error("ADT v2 helper pattern unknown case");
            if (!helper_seen.insert(which->name).second)
                throw std::runtime_error("ADT v2 helper duplicate match arm");
            r.need("->"); r.need("{"); r.need("return");
            std::int64_t return_tag = 0;
            if (!pattern.second.empty() && r.take(pattern.second)) {
                // A nullary enum is immutable and Copy. Returning its
                // arm-local borrowed identity copies a validated tag, never
                // moves an actor payload or escapes a non-Copy view.
                return_tag = which->tag;
            } else {
                const auto result = r.qualified();
                if (result.first != owner)
                    throw std::runtime_error("ADT v2 helper return foreign nominal owner");
                const auto returned = std::find_if(variants.begin(), variants.end(),
                    [&](const Variant& v) { return v.name == result.second; });
                if (returned == variants.end())
                    throw std::runtime_error("ADT v2 helper returns unknown nominal case");
                return_tag = returned->tag;
            }
            r.need(";"); r.need("}");
            helper_arms.push_back({which->tag, return_tag});
        }
        r.need("end"); r.need("}");
        if (helper_seen.size() != variants.size())
            throw std::runtime_error("ADT v2 helper nonexhaustive nominal match");
        helper_lowered = "fnc " + helper_name + "(int " + param + "): int {\n";
        for (std::size_t k = 0; k + 1 < helper_arms.size(); ++k)
            helper_lowered += "  if " + param + " == " +
                std::to_string(helper_arms[k].tag) + " then return " +
                std::to_string(helper_arms[k].returned) + "; fi\n";
        helper_lowered += "  return " +
            std::to_string(helper_arms.back().returned) + ";\n}\n";
    }
    r.need("pub"); r.need("fnc"); r.need("main"); r.need("("); r.need(")");
    r.need(":"); r.need("int"); r.need("{");
    r.need("val"); r.need(owner);
    const auto variable = r.name();
    r.need("=");
    const bool calls_helper = !helper_name.empty() && r.current().value == helper_name;
    if (calls_helper) {
        r.need(helper_name); r.need("(");
    }
    const auto ctor = r.qualified();
    if (ctor.first != owner)
        throw std::runtime_error("ADT v1 constructor has foreign nominal owner");
    const auto selected = std::find_if(variants.begin(), variants.end(),
        [&](const Variant& v) { return v.name == ctor.second; });
    if (selected == variants.end())
        throw std::runtime_error("ADT v1 unknown nominal case constructor");
    if (calls_helper) r.need(")");
    r.need(";");
    r.take("do"); r.need("match"); r.need(variable); r.need("over");
    struct Arm { std::int64_t tag, result; };
    std::vector<Arm> arms;
    std::unordered_set<std::string> seen;
    while (r.take("on")) {
        const auto q = r.nominal_pattern(owner,
            "ADT v1 match has foreign nominal owner");
        if (!q.second.empty() &&
                (q.second == variable ||
                std::any_of(variants.begin(), variants.end(),
                    [&](const Variant& v) { return v.name == q.second; })))
            throw std::runtime_error("ADT v3 match alias shadows an existing nominal binding");
        const auto which = std::find_if(variants.begin(), variants.end(),
            [&](const Variant& v) { return v.name == q.first; });
        if (which == variants.end())
            throw std::runtime_error("ADT v1 match has unknown case");
        if (!seen.insert(q.first).second)
            throw std::runtime_error("ADT v1 duplicate match arm");
        r.need("->"); r.need("{"); r.need("return");
        const auto number = r.number();
        r.need(";"); r.need("}");
        arms.push_back({which->tag, number});
    }
    r.need("end"); r.need("}");
    r.need("");
    if (seen.size() != variants.size())
        throw std::runtime_error("ADT v1 nonexhaustive nominal case match");
    // Only the checked local constructor can reach the compiler-generated
    // numeric representation; no source enum input or numeric tag API exists.
    // Keep actual branches in emitted LLVM IR; don't host-evaluate the match.
    const std::string initial_value = calls_helper
        ? helper_name + "(" + std::to_string(selected->tag) + ")"
        : std::to_string(selected->tag);
    std::string lowered = helper_lowered + "pub fnc main(): int {\n  val int " +
        variable + " = " + initial_value + ";\n";
    for (std::size_t k = 0; k + 1 < arms.size(); ++k) {
        lowered += "  if " + variable + " == " + std::to_string(arms[k].tag) +
            " then return " + std::to_string(arms[k].result) + "; fi\n";
    }
    lowered += "  return " + std::to_string(arms.back().result) + ";\n}\n";
    return lowered;
}

} // namespace oreslang::frontend
