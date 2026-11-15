// AttoLang LLVM *subset* AOT compiler, experimental and intentionally
// independent from the main frontend. No unsafe general Atomic admission.
// Exactly one stack-owned Atomic<int/bool> per zero-argument main.
// No actor, borrow, Box, FFI, reflection, GC, JIT or pointer escape.
#include "oreslang/runtime/NativeAtomicBoxV1.hpp"

#include <cctype>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

using oreslang::runtime::native_ownership_v1::Ordering;
using namespace oreslang::runtime::native_ownership_v1;

namespace {
struct Token {
    std::string text;
    std::size_t byte;
};
[[noreturn]] void fail(std::size_t byte, std::string text) {
    throw std::runtime_error("ATTO_ATOMIC_AOT_V1_REJECT at byte " +
                             std::to_string(byte) + ": " + text);
}
std::vector<Token> tokenize(std::string_view source) {
    if (source.size() > 32768) fail(0, "source exceeds 32768 byte limit");
    std::vector<Token> tokens;
    for (std::size_t i = 0; i < source.size();) {
        if (std::isspace(static_cast<unsigned char>(source[i]))) { ++i; continue; }
        if (source[i] == '/' && i + 1 < source.size() && source[i+1] == '/') {
            i += 2;
            while (i < source.size() && source[i] != '\n') ++i;
            continue;
        }
        const std::size_t start = i;
        const unsigned char ch = static_cast<unsigned char>(source[i]);
        if (std::isalpha(ch) || ch == '_') {
            ++i;
            while (i < source.size()) {
                const unsigned char c = static_cast<unsigned char>(source[i]);
                if (!std::isalnum(c) && c != '_') break;
                ++i;
            }
        } else if (std::isdigit(ch)) {
            ++i;
            while (i < source.size() &&
                   std::isdigit(static_cast<unsigned char>(source[i]))) ++i;
        } else if (std::string_view("{}().,:;#=<>-").find(source[i]) != std::string_view::npos) {
            ++i;
        } else {
            fail(i, "unexpected source token (unsupported syntax)");
        }
        tokens.push_back({std::string(source.substr(start, i - start)), start});
        if (tokens.size() > 4096) fail(i, "source token budget exceeded");
    }
    tokens.push_back({"<eof>", source.size()});
    return tokens;
}
enum class Type { Int, Bool };
struct Value { Type type; std::string ir; };

class Compiler {
    std::vector<Token> tokens_;
    std::size_t cursor_ = 0;
    std::string code_;
    std::unordered_map<std::string, Value> values_;
    std::string atomic_name_;
    Type atomic_type_ = Type::Int;
    bool has_atomic_ = false;
    unsigned step_ = 0;
    unsigned statements_ = 0;

    const Token& at() const { return tokens_.at(cursor_); }
    bool take(std::string_view word) {
        if (at().text != word) return false;
        ++cursor_;
        return true;
    }
    void need(std::string_view word) {
        if (!take(word)) fail(at().byte, "expected '" + std::string(word) + "'");
    }
    static bool is_identifier(std::string_view name) {
        if (name.empty()) return false;
        auto first = static_cast<unsigned char>(name[0]);
        if (!std::isalpha(first) && first != '_') return false;
        for (unsigned char c : name)
            if (!std::isalnum(c) && c != '_') return false;
        return true;
    }
    std::string ident() {
        const Token t = at();
        if (!is_identifier(t.text) || t.text == "true" || t.text == "false")
            fail(t.byte, "expected identifier");
        ++cursor_;
        return t.text;
    }
    Type type() {
        if (take("int")) return Type::Int;
        if (take("bool")) return Type::Bool;
        fail(at().byte, "only int and bool are admitted in Atomic AOT v1");
    }
    std::string new_temp() { return "%atomic." + std::to_string(++step_); }
    std::string scalar_type() const { return atomic_type_ == Type::Int ? "i64" : "i8"; }
    std::string alignment() const { return atomic_type_ == Type::Int ? "8" : "1"; }
    void emit(std::string line) { code_ += "  " + std::move(line) + "\n"; }

    Value value() {
        if (take("true")) return {Type::Bool, "true"};
        if (take("false")) return {Type::Bool, "false"};
        const bool negative = take("-");
        if (std::isdigit(static_cast<unsigned char>(at().text[0]))) {
            const Token t = at();
            ++cursor_;
            std::uint64_t magnitude = 0;
            const auto [end, error] = std::from_chars(
                t.text.data(), t.text.data() + t.text.size(), magnitude);
            if (error != std::errc{} || end != t.text.data() + t.text.size() ||
                magnitude > static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max()) + (negative ? 1u : 0u))
                fail(t.byte, "int literal out of i64 range");
            if (negative) {
                if (magnitude == static_cast<std::uint64_t>(
                        std::numeric_limits<std::int64_t>::max()) + 1u)
                    return {Type::Int, "-9223372036854775808"};
                return {Type::Int, "-" + t.text};
            }
            return {Type::Int, t.text};
        }
        if (negative) fail(at().byte, "signed value must be an integer literal");
        const auto name = ident();
        const auto found = values_.find(name);
        if (found == values_.end())
            fail(at().byte, "unknown, moved, or opaque scalar '" + name + "'");
        return found->second;
    }
    Value typed_value() {
        const auto v = value();
        if (v.type != atomic_type_)
            fail(at().byte, "atomic payload type mismatch");
        return v;
    }
    std::string native_value(const Value& v) {
        if (atomic_type_ == Type::Int) return v.ir;
        const auto cast = new_temp();
        emit(cast + " = zext i1 " + v.ir + " to i8");
        return cast;
    }
    Ordering parse_order() {
        need("#");
        const auto s = ident();
        if (s == "relaxed") return Ordering::Relaxed;
        if (s == "acquire") return Ordering::Acquire;
        if (s == "release") return Ordering::Release;
        if (s == "acq_rel") return Ordering::AcqRel;
        if (s == "seq_cst") return Ordering::SeqCst;
        fail(at().byte, "unknown atomic memory ordering");
    }
    static std::string llvm_order(Ordering order) {
        switch (order) {
        case Ordering::Relaxed: return "monotonic";
        case Ordering::Acquire: return "acquire";
        case Ordering::Release: return "release";
        case Ordering::AcqRel: return "acq_rel";
        case Ordering::SeqCst: return "seq_cst";
        }
        throw std::logic_error("invalid memory order");
    }
    Ordering optional_order() {
        if (take(")")) return Ordering::SeqCst;
        const auto o = parse_order();
        need(")");
        return o;
    }
    Value read_result(const std::string& register_name) {
        if (atomic_type_ == Type::Int) return {Type::Int, register_name};
        const auto flag = new_temp();
        emit(flag + " = icmp ne i8 " + register_name + ", 0");
        return {Type::Bool, flag};
    }
    Value operation() {
        const auto owner = ident();
        if (!has_atomic_ || owner != atomic_name_)
            fail(at().byte, "only the verified local Atomic owner may be used");
        need(".");
        const auto op = ident();
        need("(");
        const std::string llvm_type = scalar_type();
        if (op == "load") {
            const auto order = optional_order();
            if (!is_load_order(order)) fail(at().byte, "invalid Atomic.load memory order");
            const auto v = new_temp();
            emit(v + " = load atomic " + llvm_type +
                 ", ptr %cell " + llvm_order(order) + ", align " + alignment());
            return read_result(v);
        }
        if (op == "store") {
            const auto arg = typed_value();
            Ordering order = Ordering::SeqCst;
            if (take(",")) order = parse_order();
            need(")");
            if (!is_store_order(order)) fail(at().byte, "invalid Atomic.store memory order");
            emit("store atomic " + llvm_type + " " + native_value(arg) +
                 ", ptr %cell " + llvm_order(order) + ", align " + alignment());
            return {Type::Int, ""}; // void operation, cannot bind as value
        }
        if (op == "compare_exchange") {
            const auto expected = typed_value();
            need(",");
            const auto desired = typed_value();
            Ordering success = Ordering::SeqCst, failure = Ordering::SeqCst;
            if (take(",")) {
                success = parse_order();
                need(",");
                failure = parse_order();
            }
            need(")");
            if (!is_cas_order(success, failure))
                fail(at().byte, "invalid compare_exchange success/failure ordering");
            const auto a = native_value(expected);
            const auto b = native_value(desired);
            const auto result = new_temp();
            emit(result + " = cmpxchg ptr %cell, " + llvm_type + " " + a +
                 ", " + llvm_type + " " + b + " " + llvm_order(success) +
                 " " + llvm_order(failure));
            const auto ok = new_temp();
            emit(ok + " = extractvalue { " + llvm_type + ", i1 } " + result + ", 1");
            return {Type::Bool, ok};
        }
        static const std::unordered_map<std::string, std::string> rmw{
            {"swap","xchg"},{"fetch_add","add"},{"fetch_sub","sub"},
            {"fetch_and","and"},{"fetch_or","or"},{"fetch_xor","xor"}
        };
        auto it = rmw.find(op);
        if (it == rmw.end()) fail(at().byte, "unrecognized or forbidden Atomic operation");
        if (atomic_type_ == Type::Bool && op != "swap")
            fail(at().byte, "Atomic<bool> permits swap, load, store and compare_exchange only");
        const auto arg = typed_value();
        Ordering order = Ordering::SeqCst;
        if (take(",")) order = parse_order();
        need(")");
        if (!is_rmw_order(order)) fail(at().byte, "invalid atomic RMW memory order");
        const auto raw = native_value(arg);
        const auto prev = new_temp();
        emit(prev + " = atomicrmw " + it->second + " ptr %cell, " +
             llvm_type + " " + raw + " " + llvm_order(order));
        return read_result(prev);
    }
    bool return_statement() {
        if (!take("return")) return false;
        Value v;
        if (has_atomic_ && at().text == atomic_name_) v = operation();
        else v = value();
        if (v.type != Type::Int || v.ir.empty())
            fail(at().byte, "main must return a scalar int");
        need(";");
        const auto r = new_temp();
        emit(r + " = trunc i64 " + v.ir + " to i32");
        emit("ret i32 " + r);
        return true;
    }
    void declaration() {
        need("val");
        if (!has_atomic_) {
            need("Atomic"); need("<");
            atomic_type_ = type(); need(">");
            atomic_name_ = ident();
            if (atomic_name_ == "main") fail(at().byte, "reserved owner name");
            need("=");
            need("Atomic"); need("<");
            const auto initialized_type = type(); need(">");
            if (initialized_type != atomic_type_) fail(at().byte, "Atomic constructor type mismatch");
            need("(");
            const auto init = typed_value();
            need(")"); need(";");
            has_atomic_ = true;
            emit("%cell = alloca " + scalar_type() + ", align " + alignment());
            emit("store atomic " + scalar_type() + " " + native_value(init) +
                 ", ptr %cell seq_cst, align " + alignment());
            return;
        }
        const auto declared = type();
        const auto name = ident();
        if (name == atomic_name_ || values_.count(name))
            fail(at().byte, "duplicate or shadowed scalar name");
        need("=");
        const auto v = operation();
        if (v.ir.empty() || v.type != declared)
            fail(at().byte, "incorrect typed Atomic result or void value");
        need(";");
        values_.emplace(name, v);
    }
public:
    explicit Compiler(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}
    std::string compile() {
        need("fnc"); need("main"); need("("); need(")"); need(":"); need("int"); need("{");
        code_ = "define i32 @main() {\nentry:\n";
        while (!return_statement()) {
            if (++statements_ > 64) fail(at().byte, "statement budget exceeded");
            if (at().text == "val") declaration();
            else if (has_atomic_ && at().text == atomic_name_) {
                const auto result = operation();
                if (!result.ir.empty()) fail(at().byte, "non-void Atomic result cannot be discarded");
                need(";");
            } else if (at().text == "Box") {
                fail(at().byte, "Box<T> requires distinct borrow and owned-drop compiler lowering");
            } else {
                fail(at().byte, "unsupported statement; Atomic AOT v1 is closed-world");
            }
        }
        if (!has_atomic_) fail(at().byte, "expected one owned Atomic<T> local");
        need("}");
        need("<eof>");
        code_ += "}\n";
        return code_;
    }
};
} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: atto-atomic-aot-v1 input.atto output.ll\n";
        return 2;
    }
    try {
        std::ifstream input(argv[1], std::ios::binary);
        if (!input) throw std::runtime_error("cannot read input .atto file");
        const std::string source(
            std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{});
        Compiler compiler(tokenize(source));
        const std::string llvm_ir = compiler.compile(); // validate before file is opened
        std::ofstream output(argv[2], std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("cannot create output LLVM IR");
        output << llvm_ir;
        if (!output) throw std::runtime_error("failed writing LLVM IR");
        std::cout << "ATTO_ATOMIC_AOT_V1_IR_OK\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << "\n";
        return 1;
    }
}
