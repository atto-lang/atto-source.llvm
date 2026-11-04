#pragma once
// Deliberately opt-in FIRST POSITIVE GUEST SLICE, not full inverted types.
// Compiles a bounded Oreslang i64 inverted struct and its explicit InvertedArray
// into physical LLVM IR columns. Main compiler admission remains fail-closed.
// This source-only adapter must be retired when the typed HIR/MIR path lands.
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include "oreslang/types/InvertedTypedPlan.hpp"
#include "oreslang/types/InvertedTypedMir.hpp"
#include "oreslang/types/InvertedMirLlvmI64.hpp"
#include <stdexcept>
#include <system_error>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace oreslang::experimental_inverted_i64 {
namespace detail {
struct Token {
    std::string text;
    std::size_t offset;
    bool identifier = false;
    bool number = false;
};

inline bool letter(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}
inline bool digit(char c) { return c >= '0' && c <= '9'; }
inline bool space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}
inline std::vector<Token> tokenize(std::string_view source) {
    if (source.size() > 65536)
        throw std::runtime_error("experimental inverted i64 source exceeds 64 KiB");
    std::vector<Token> out;
    for (std::size_t at = 0; at < source.size();) {
        if (out.size() >= 4096) throw std::runtime_error("inverted token budget exceeded");
        if (space(source[at])) { ++at; continue; }
        if (source[at] == '/' && at + 1 < source.size() && source[at + 1] == '/') {
            at += 2;
            while (at < source.size() && source[at] != '\n' && source[at] != '\r') ++at;
            continue;
        }
        if (source[at] == '/' && at + 1 < source.size() && source[at + 1] == '*') {
            const auto start = at;
            at += 2;
            unsigned depth = 1;
            while (depth && at < source.size()) {
                if (at + 1 < source.size() && source[at] == '/' && source[at + 1] == '*') {
                    if (++depth > 128) throw std::runtime_error("inverted block comment too deep");
                    at += 2;
                } else if (at + 1 < source.size() && source[at] == '*' && source[at + 1] == '/') {
                    --depth; at += 2;
                } else ++at;
            }
            if (depth) throw std::runtime_error(
                "unterminated inverted block comment at byte " + std::to_string(start));
            continue;
        }
        const auto start = at;
        if (letter(source[at])) {
            while (at < source.size() && (letter(source[at]) || digit(source[at]))) ++at;
            out.push_back({std::string(source.substr(start, at - start)), start, true, false});
        } else if (digit(source[at])) {
            while (at < source.size() && digit(source[at])) ++at;
            out.push_back({std::string(source.substr(start, at - start)), start, false, true});
        } else {
            const char c = source[at++];
            if (std::string_view("<>(){}:;,.=+-").find(c) == std::string_view::npos)
                throw std::runtime_error("unsupported inverted token at byte " + std::to_string(start));
            out.push_back({std::string(1, c), start, false, false});
        }
    }
    out.push_back({"<eof>", source.size(), false, false});
    return out;
}
inline std::string number(std::int64_t v) { return std::to_string(v); }

class Compiler {
    std::vector<Token> tokens_;
    std::size_t at_ = 0;
    std::string type_, owner_;
    std::vector<std::string> fields_;
    oreslang::InvertedStructDecl typed_decl_;
    std::optional<oreslang::dop::hir::LinearPlan> typed_plan_;
    std::vector<std::array<std::int64_t, 2>> mir_rows_;
    std::vector<std::int64_t> mir_writes_;
    std::size_t capacity_ = 0;
    std::string instructions_;
    std::size_t serial_ = 0;
    bool returned_ = false;

    const Token& cur() const { return tokens_.at(at_); }
    bool accept(std::string_view s) {
        if (cur().text != s) return false;
        ++at_; return true;
    }
    [[noreturn]] void fail(const std::string& reason) const {
        throw std::runtime_error("experimental inverted i64: " + reason +
                                 " at byte " + std::to_string(cur().offset));
    }
    void need(std::string_view s) {
        if (!accept(s)) fail("expected '" + std::string(s) + "'");
    }
    std::string identifier() {
        if (!cur().identifier) fail("expected identifier");
        auto name = cur().text; ++at_; return name;
    }
    std::int64_t integer() {
        const bool negative = accept("-");
        if (!cur().number) fail("expected signed i64 literal");
        const std::string digits = cur().text; ++at_;
        std::uint64_t magnitude = 0;
        auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), magnitude);
        const auto bound = static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max()) + (negative ? 1u : 0u);
        if (ec != std::errc{} || ptr != digits.data() + digits.size() ||
            magnitude > bound) fail("i64 integer literal overflow");
        if (negative && magnitude == bound) return std::numeric_limits<std::int64_t>::min();
        return negative ? -static_cast<std::int64_t>(magnitude)
                        : static_cast<std::int64_t>(magnitude);
    }
    std::size_t index() {
        const auto v = integer();
        if (v < 0) fail("inverted index must be nonnegative");
        if (static_cast<std::uint64_t>(v) >
            static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
            fail("inverted index exceeds target size_t");
        return static_cast<std::size_t>(v);
    }
    std::size_t field() {
        const auto name = identifier();
        for (std::size_t i = 0; i < fields_.size(); ++i)
            if (fields_[i] == name) return i;
        fail("unknown inverted field '" + name + "'");
    }
    void add(const std::string& line) {
        instructions_ += "  " + line + "\n";
    }
    std::string temp() { return "%t" + std::to_string(++serial_); }
    std::string label() { return "guard" + std::to_string(++serial_); }
    void guard(const std::string& value, const std::string& limit) {
        const auto condition = temp();
        const auto good = label(), bad = label();
        add(condition + " = icmp ult i64 " + value + ", " + limit);
        add("br i1 " + condition + ", label %" + good + ", label %" + bad);
        instructions_ += bad + ":\n";
        add("call void @llvm.trap()");
        add("unreachable");
        instructions_ += good + ":\n";
    }
    std::string pointer(std::size_t col, const std::string& idx) {
        const auto p = temp();
        add(p + " = getelementptr inbounds [" + std::to_string(capacity_) +
            " x i64], ptr %col" + std::to_string(col) +
            ", i64 0, i64 " + idx);
        return p;
    }
    std::string row_index(std::size_t idx) {
        const auto length = temp();
        add(length + " = load i64, ptr %length");
        guard(std::to_string(idx), length);
        return std::to_string(idx);
    }
    void definition() {
        // Both canonical orders for pub are accepted. No second declaration
        // or ordinary/untyped collection fallback is available in this mode.
        const bool public_before = accept("pub");
        need("define");
        const bool public_after = accept("pub");
        if (public_before && public_after) fail("duplicate pub");
        need("inverted");
        need("struct");
        type_ = identifier();
        typed_decl_.name = type_;
        typed_decl_.exported = public_before || public_after;
        need("as");
        std::unordered_set<std::string> names;
        while (cur().text != "end") {
            if (fields_.size() == 8) fail("inverted i64 field budget exceeded");
            const bool is_mutable = accept("let");
            const auto field_offset = cur().offset;
            const auto name = identifier();
            if (!names.insert(name).second) fail("duplicate inverted field");
            need(":");
            // Only one primitive is independently lowered in this first
            // source slice. Packed nested records and classes still reject.
            need("int");
            need(";");
            fields_.push_back(name);
            typed_decl_.fields.push_back(oreslang::InvertedStructField{
                name, oreslang::InvertedScalarKind::I64, is_mutable, field_offset});

        }
        if (fields_.empty()) fail("empty inverted struct");
        need("end");
    }
    void declare_owner() {
        need("let");
        owner_ = identifier();
        need("=");
        need("InvertedArray");
        need("<");
        const auto type_name = identifier();
        if (type_name != type_) fail("inverted collection nominal type mismatch");
        need(">");
        need(".");
        need("with_capacity");
        need("(");
        const auto cap = integer();
        if (cap <= 0 || cap > 256) fail("inverted capacity must be 1..256");
        capacity_ = static_cast<std::size_t>(cap);
        if (capacity_ * fields_.size() > 1024) fail("inverted row cell budget exceeded");
        need(")");
        need(";");
        // Actual guest source reuses the same typed one-level compiler plan
        // as the production AST. Runtime row bounds remain LLVM guards.
        typed_plan_.emplace(
            oreslang::dop::hir::Layout::from_decl(typed_decl_), capacity_);
        add("%length = alloca i64");
        add("store i64 0, ptr %length");
        // A bounded, stack-local logical owner is revoked on return for
        // all admitted field arities, including the legacy 1/3..8 slice.
        add("%alive = alloca i1");
        add("store i1 true, ptr %alive");
        for (std::size_t i = 0; i < fields_.size(); ++i) {
            add("%col" + std::to_string(i) + " = alloca [" +
                std::to_string(capacity_) + " x i64]");
        }
    }
    void action() {
        const auto obj = identifier();
        if (obj != owner_) fail("unknown inverted collection owner");
        need(".");
        if (accept("push")) {
            need("(");
            std::vector<std::int64_t> values;
            for (std::size_t i = 0; i < fields_.size(); ++i) {
                if (i) need(",");
                values.push_back(integer());
            }
            need(")"); need(";");
            typed_plan_->push(values.size());
            // The two-column slice uses verified MIR as the sole source of
            // LLVM IR; retain each guest operand in source evaluation order.
            if (fields_.size() == 2)
                mir_rows_.push_back({values[0], values[1]});
            const auto length = temp();
            add(length + " = load i64, ptr %length");
            guard(length, std::to_string(capacity_));
            for (std::size_t i = 0; i < fields_.size(); ++i)
                add("store i64 " + number(values[i]) + ", ptr " + pointer(i, length));
            const auto next = temp();
            add(next + " = add i64 " + length + ", 1");
            add("store i64 " + next + ", ptr %length");
        } else if (accept("set")) {
            need("(");
            const auto idx = index();
            need(",");
            const auto col = field();
            need(",");
            const auto value = integer();
            need(")"); need(";");
            typed_plan_->write_runtime_checked(idx, fields_[col]);
            if (fields_.size() == 2) mir_writes_.push_back(value);
            const auto address = pointer(col, row_index(idx));
            add("store i64 " + number(value) + ", ptr " + address);
        } else {
            fail("only push and set supported as inverted statements");
        }
    }
    void finish() {
        need("return");
        const auto obj = identifier();
        if (obj != owner_) fail("unknown inverted collection result owner");
        need("."); need("get"); need("(");
        const auto idx = index();
        need(",");
        const auto col = field();
        need(")"); need(";");
        typed_plan_->read_runtime_checked(idx, fields_[col]);
        const auto address = pointer(col, row_index(idx));
        const auto loaded = temp();
        add(loaded + " = load i64, ptr " + address);
        add("%result = trunc i64 " + loaded + " to i32");
        // Return only after owner revocation and physical-column scrub.
        returned_ = true;
    }
public:
    explicit Compiler(std::string_view source):tokens_(tokenize(source)) {}
    std::string compile() {
        definition();
        need("fnc"); need("main"); need("("); need(")"); need(":"); need("int"); need("{");
        declare_owner();
        while (cur().text != "return") action();
        finish();
        need("}");
        if (cur().text != "<eof>" || !returned_) fail("trailing unsupported inverted code");
        // All bounded inverted owners die before the source function returns.
        // The two-column checked MIR emitter and the restricted legacy 1/3..8
        // adapter each consume the same terminal Close obligation.
        typed_plan_->close();
        // Independently recheck the whole source operation stream as typed
        // MIR before returning ANY LLVM IR. Main production codegen still
        // rejects these declarations; this is explicitly opt-in.
        const auto verified_mir = oreslang::dop::mir::Module::lower(*typed_plan_);
        if (fields_.size() == 2) {
            // Real source -> checked HIR -> independently verified MIR ->
            // emitter-owned physical LLVM IR. This path remains opt-in:
            // regular production source admission stays fail closed.
            return oreslang::dop::mir::emit_native_i64_pair_ir(
                verified_mir, oreslang::dop::mir::I64PairIrOperands{
                    mir_rows_, mir_writes_});
        }
        // One or 3..8 columns still use the older, explicitly experimental
        // checked source adapter; do not imply general MIR codegen parity.
        std::size_t expected_bounds = 0;
        for (const auto& op : verified_mir.instructions()) {
            if (op.kind == oreslang::dop::hir::OpKind::Push ||
                op.kind == oreslang::dop::hir::OpKind::ReadField ||
                op.kind == oreslang::dop::hir::OpKind::WriteField)
                ++expected_bounds;
        }
        // This narrow source adapter emits one signed-unsigned index guard
        // per admitted push/get/set. Require the same guard count as the MIR
        // operations rather than silently publishing unchecked IR.
        std::size_t emitted_bounds = 0;
        for (std::size_t at = 0;
             (at = instructions_.find(" = icmp ult i64 ", at)) != std::string::npos;
             at += sizeof(" = icmp ult i64 ") - 1)
            ++emitted_bounds;
        if (emitted_bounds != expected_bounds)
            throw std::logic_error("inverted MIR bounds obligation was not lowered");
        if (verified_mir.instructions().back().kind != oreslang::dop::hir::OpKind::Close)
            throw std::logic_error("legacy inverted guest requires terminal owner close");
        // A detached scalar SSA result survives teardown, while all bounded
        // stack cells are overwritten with volatile stores before return.
        // No handles, borrows, actor ownership or zero-copy ABI are exposed.
        add("store volatile i1 false, ptr %alive");
        for (std::size_t field = 0; field < fields_.size(); ++field)
            for (std::size_t row = 0; row < capacity_; ++row)
                add("store volatile i64 0, ptr " + pointer(field, std::to_string(row)));
        add("store volatile i64 0, ptr %length");
        add("ret i32 %result");
        std::string ir = "; generated by experimental checked inverted i64 guest compiler\n";
        ir += "declare void @llvm.trap()\n";
        ir += "define i32 @main() {\nentry:\n";
        ir += instructions_;
        ir += "}\n";
        return ir;
    }
};
} // namespace detail

inline std::string compile(std::string_view source) {
    return detail::Compiler(source).compile();
}
} // namespace oreslang::experimental_inverted_i64
