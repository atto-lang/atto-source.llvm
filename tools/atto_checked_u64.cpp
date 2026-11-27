// Shared closed unsigned scalar frontend. LLVM and native C++ emitters use one validated tree.
#include <charconv>
#include <cstdint>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
[[noreturn]] void fail(const char* why) { throw std::runtime_error(why); }
bool digit(char c) { return c >= '0' && c <= '9'; }
bool alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
struct Node {
    enum Kind { literal, param, call, binary } kind;
    std::string value;
    std::vector<std::unique_ptr<Node>> children;
};
struct Function {
    std::string name;
    bool exported;
    std::vector<std::string> params;
    std::unique_ptr<Node> body;
    unsigned state = 0;
    unsigned cost = 0;
};
class Compiler {
    std::vector<std::string> tokens;
    std::size_t pos = 0;
    unsigned nodes = 0;
    bool uses_division = false;
    std::vector<Function> funcs;
    std::map<std::string, std::size_t> index;
    std::string_view peek() const { return pos < tokens.size() ? tokens[pos] : std::string_view{}; }
    bool eat(std::string_view s) { if (peek() != s) return false; ++pos; return true; }
    void need(std::string_view s) { if (!eat(s)) fail("unexpected token"); }
    std::string name() {
        auto s = peek();
        if (s.empty() || !alpha(s.front()) || s.size() > 64 || s == "fnc" || s == "pub" || s == "return" || s == "u64" || s == "uint") fail("invalid identifier");
        ++pos; return std::string(s);
    }
    void type() { if (!eat("u64") && !eat("uint")) fail("unsigned type required"); }
    std::unique_ptr<Node> make(Node::Kind kind, std::string value) {
        if (++nodes > 1024) fail("node budget");
        return std::make_unique<Node>(Node{kind, std::move(value), {}});
    }
    std::unique_ptr<Node> expression(unsigned depth = 0, int min = 1) {
        if (depth > 32) fail("expression depth");
        std::unique_ptr<Node> lhs;
        if (eat("(")) { lhs = expression(depth + 1); need(")"); }
        else if (!peek().empty() && digit(peek().front())) {
            auto s = peek(); std::uint64_t value = 0;
            auto result = std::from_chars(s.data(), s.data() + s.size(), value);
            if (result.ec != std::errc{} || result.ptr != s.data() + s.size() || (s.size() > 1 && s.front() == '0')) fail("literal outside u64 or noncanonical");
            ++pos; lhs = make(Node::literal, std::to_string(value));
        } else {
            auto id = name(); lhs = make(Node::param, std::move(id));
            if (eat("(")) {
                lhs->kind = Node::call;
                if (!eat(")")) {
                    do { if (lhs->children.size() == 16) fail("arity budget"); lhs->children.push_back(expression(depth + 1)); } while (eat(","));
                    need(")");
                }
            }
        }
        for (;;) {
            auto op = peek(); int prec = op == "*" || op == "/" ? 2 : op == "+" || op == "-" ? 1 : 0;
            if (prec < min) break;
            if (op == "/") uses_division = true;
            ++pos; auto n = make(Node::binary, std::string(op));
            n->children.push_back(std::move(lhs)); n->children.push_back(expression(depth + 1, prec + 1)); lhs = std::move(n);
        }
        return lhs;
    }
    unsigned check_node(const Node& n, const Function& f, unsigned depth) {
        if (depth > 64) fail("tree depth");
        unsigned work = 1;
        if (n.kind == Node::param) {
            bool found = false; for (const auto& p : f.params) if (p == n.value) found = true;
            if (!found) fail("unknown parameter");
        }
        for (const auto& child : n.children) { work += check_node(*child, f, depth + 1); if (work > 10000) fail("work budget"); }
        if (n.kind == Node::call) {
            auto i = index.find(n.value);
            if (i == index.end() || funcs[i->second].params.size() != n.children.size()) fail("unknown call or arity");
            work += check_function(i->second);
        }
        if (work > 10000) fail("work budget");
        return work;
    }
    unsigned check_function(std::size_t i) {
        auto& f = funcs[i];
        if (f.state == 1) fail("recursive call graph");
        if (f.state == 2) return f.cost;
        f.state = 1; f.cost = check_node(*f.body, f, 0); f.state = 2; return f.cost;
    }
    std::string emit_node(const Node& n, const Function& f, std::string& out, unsigned& next) const {
        if (n.kind == Node::literal) return n.value;
        if (n.kind == Node::param) {
            for (std::size_t i = 0; i < f.params.size(); ++i) if (f.params[i] == n.value) return "%p" + std::to_string(i);
            fail("internal parameter");
        }
        std::vector<std::string> args;
        for (const auto& child : n.children) args.push_back(emit_node(*child, f, out, next));
        std::string target;
        if (n.kind == Node::call) target = "f" + std::to_string(index.at(n.value));
        else target = n.value == "+" ? "checked_add" : n.value == "-" ? "checked_sub" : n.value == "*" ? "checked_mul" : "checked_div";
        auto v = "%v" + std::to_string(next++);
        out += "  " + v + " = call i64 @" + target + "(";
        for (std::size_t i = 0; i < args.size(); ++i) out += (i ? ", i64 " : "i64 ") + args[i];
        out += ")\n"; return v;
    }
    std::string emit_cpp_node(const Node& n, const Function& f, std::string& out, unsigned& next) const {
        if (n.kind == Node::literal) return n.value + "ULL";
        if (n.kind == Node::param) {
            for (std::size_t i = 0; i < f.params.size(); ++i) if (f.params[i] == n.value) return "p" + std::to_string(i);
            fail("internal parameter");
        }
        std::vector<std::string> args;
        for (const auto& child : n.children) args.push_back(emit_cpp_node(*child, f, out, next));
        std::string target;
        if (n.kind == Node::call) target = "f" + std::to_string(index.at(n.value));
        else target = n.value == "+" ? "checked_add" : n.value == "-" ? "checked_sub" : n.value == "*" ? "checked_mul" : "checked_div";
        auto v = "v" + std::to_string(next++);
        out += "  const U " + v + " = " + target + "(";
        for (std::size_t i = 0; i < args.size(); ++i) out += (i ? ", " : "") + args[i];
        out += ");\n"; return v;
    }
public:
    explicit Compiler(const std::string& source) {
        if (source.size() > 16384) fail("source budget");
        for (std::size_t i = 0; i < source.size();) {
            char c = source[i];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { ++i; continue; }
            auto start = i++;
            if (alpha(c)) { while (i < source.size() && (alpha(source[i]) || digit(source[i]))) ++i; }
            else if (digit(c)) { while (i < source.size() && digit(source[i])) ++i; }
            else if (std::string_view("(){}:,;+-*/").find(c) == std::string_view::npos) fail("unsupported source byte");
            tokens.push_back(source.substr(start, i - start));
            if (tokens.size() > 2048) fail("token budget");
        }
        while (pos < tokens.size()) {
            if (funcs.size() == 32) fail("function budget");
            bool pub = eat("pub"); need("fnc"); auto id = name();
            if (index.contains(id)) fail("duplicate function");
            Function f{std::move(id), pub, {}, nullptr};
            need("(");
            if (!eat(")")) {
                do {
                    if (f.params.size() == 16) fail("arity budget");
                    type(); auto p = name(); for (const auto& other : f.params) if (p == other) fail("duplicate parameter");
                    f.params.push_back(std::move(p));
                } while (eat(",")); need(")");
            }
            need(":"); type(); need("{"); need("return"); f.body = expression(); need(";"); need("}");
            index.emplace(f.name, funcs.size()); funcs.push_back(std::move(f));
        }
        auto m = index.find("main");
        if (m == index.end() || !funcs[m->second].exported || !funcs[m->second].params.empty()) fail("public zero-arity main required");
        for (std::size_t i = 0; i < funcs.size(); ++i) check_function(i);
    }
    std::string emit_llvm() const {
        std::string out = "declare void @llvm.trap()\n";
        for (auto op : {"add", "sub"}) {
            std::string s(op), intrinsic = "@llvm.u" + s + ".with.overflow.i64";
            out += "declare { i64, i1 } " + intrinsic + "(i64, i64)\n";
            out += "define internal i64 @checked_" + s + "(i64 %a, i64 %b) {\nentry:\n  %r = call { i64, i1 } " + intrinsic + "(i64 %a, i64 %b)\n  %bad = extractvalue { i64, i1 } %r, 1\n  br i1 %bad, label %overflow, label %ok\noverflow:\n  call void @llvm.trap()\n  unreachable\nok:\n  %v = extractvalue { i64, i1 } %r, 0\n  ret i64 %v\n}\n";
        }
        // Multiply 32-bit limbs: no wide division or wide multiply helper.
        // Each partial product fits in 64 bits. Reject any carry above bit 63.
        out += R"(define internal i64 @checked_mul(i64 %a, i64 %b) {
entry:
  %al = and i64 %a, 4294967295
  %bl = and i64 %b, 4294967295
  %ah = lshr i64 %a, 32
  %bh = lshr i64 %b, 32
  %an = icmp ne i64 %ah, 0
  %bn = icmp ne i64 %bh, 0
  %both = and i1 %an, %bn
  br i1 %both, label %overflow, label %partial
partial:
  %lo = mul i64 %al, %bl
  %cross1 = mul i64 %ah, %bl
  %cross2 = mul i64 %al, %bh
  %bad1 = icmp ugt i64 %cross1, 4294967295
  %bad2 = icmp ugt i64 %cross2, 4294967295
  %badcross = or i1 %bad1, %bad2
  br i1 %badcross, label %overflow, label %carry
carry:
  %lohi = lshr i64 %lo, 32
  %cross = add i64 %cross1, %cross2
  %hi = add i64 %cross, %lohi
  %badhi = icmp ugt i64 %hi, 4294967295
  br i1 %badhi, label %overflow, label %ok
overflow:
  call void @llvm.trap()
  unreachable
ok:
  %low = and i64 %lo, 4294967295
  %upper = shl i64 %hi, 32
  %v = or i64 %upper, %low
  ret i64 %v
}
)";
        if (uses_division) out += R"(define internal i64 @checked_div(i64 %a, i64 %b) {
entry:
  %zero = icmp eq i64 %b, 0
  br i1 %zero, label %fault, label %loop
fault:
  call void @llvm.trap()
  unreachable
loop:
  %i = phi i32 [ 0, %entry ], [ %next, %loop ]
  %bits = phi i64 [ %a, %entry ], [ %bits2, %loop ]
  %r = phi i64 [ 0, %entry ], [ %r2, %loop ]
  %q = phi i64 [ 0, %entry ], [ %q2, %loop ]
  %carry = icmp slt i64 %r, 0
  %bit = lshr i64 %bits, 63
  %shift = shl i64 %r, 1
  %trial = or i64 %shift, %bit
  %ge = icmp uge i64 %trial, %b
  %take = or i1 %carry, %ge
  %sub = sub i64 %trial, %b
  %r2 = select i1 %take, i64 %sub, i64 %trial
  %qshift = shl i64 %q, 1
  %qbit = zext i1 %take to i64
  %q2 = or i64 %qshift, %qbit
  %bits2 = shl i64 %bits, 1
  %next = add i32 %i, 1
  %done = icmp eq i32 %next, 64
  br i1 %done, label %exit, label %loop
exit:
  ret i64 %q2
}
)";
        for (std::size_t i = 0; i < funcs.size(); ++i) {
            const auto& f = funcs[i]; out += "define internal i64 @f" + std::to_string(i) + "(";
            for (std::size_t p = 0; p < f.params.size(); ++p) out += (p ? ", i64 %p" : "i64 %p") + std::to_string(p);
            out += ") {\nentry:\n"; unsigned next = 0; auto v = emit_node(*f.body, f, out, next); out += "  ret i64 " + v + "\n}\n";
        }
        out += "define i64 @atto_guest_main() {\nentry:\n  %v = call i64 @f" + std::to_string(index.at("main")) + "()\n  ret i64 %v\n}\n";
        if (out.size() > 131072) fail("emitted IR budget");
        return out;
    }
    std::string emit_cpp() const {
        std::string out = R"(#include <cstdint>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <limits>
using U = std::uint64_t;
constexpr U MAX = std::numeric_limits<U>::max();
[[noreturn]] void overflow() { std::fputs("E_U64_OVERFLOW\n", stderr); std::exit(70); }
[[maybe_unused]] static U checked_add(U a, U b) { if (b > MAX - a) overflow(); return a + b; }
[[maybe_unused]] static U checked_sub(U a, U b) { if (b > a) overflow(); return a - b; }
[[maybe_unused]] static U checked_mul(U a, U b) {
    constexpr U MASK = 0xffffffffULL;
    const U al = a & MASK, bl = b & MASK, ah = a >> 32, bh = b >> 32;
    if (ah != 0 && bh != 0) overflow();
    const U lo = al * bl, cross1 = ah * bl, cross2 = al * bh;
    if (cross1 > MASK || cross2 > MASK) overflow();
    const U hi = cross1 + cross2 + (lo >> 32);
    if (hi > MASK) overflow();
    return (hi << 32) | (lo & MASK);
}
)";
        if (uses_division) out += R"([[noreturn]] void division_by_zero() { std::fputs("E_U64_DIVISION_BY_ZERO\n", stderr); std::exit(71); }
[[maybe_unused]] static U checked_div(U a, U b) {
    if (b == 0) division_by_zero();
    U q = 0, r = 0;
    for (unsigned i = 0; i < 64; ++i) {
        const bool carry = (r >> 63) != 0;
        const U trial = (r << 1) | (a >> 63);
        const bool take = carry || trial >= b;
        r = take ? trial - b : trial;
        q = (q << 1) | static_cast<U>(take);
        a <<= 1;
    }
    return q;
}
)";
        for (std::size_t i = 0; i < funcs.size(); ++i) {
            out += "[[maybe_unused]] static U f" + std::to_string(i) + "(";
            for (std::size_t p = 0; p < funcs[i].params.size(); ++p) out += (p ? ", U" : "U");
            out += ");\n";
        }
        for (std::size_t i = 0; i < funcs.size(); ++i) {
            const auto& f = funcs[i]; out += "static U f" + std::to_string(i) + "(";
            for (std::size_t p = 0; p < f.params.size(); ++p) out += (p ? ", [[maybe_unused]] U p" : "[[maybe_unused]] U p") + std::to_string(p);
            out += ") {\n"; unsigned next = 0; auto v = emit_cpp_node(*f.body, f, out, next); out += "  return " + v + ";\n}\n";
        }
        out += "int main() { std::printf(\"%\" PRIu64 \"\\n\", f" + std::to_string(index.at("main")) + "()); return 0; }\n";
        if (out.size() > 131072) fail("emitted C++ budget");
        return out;
    }
};
} // namespace
int main(int argc, char** argv) {
    try {
        bool cpp = argc == 2 && std::string_view(argv[1]) == "--emit=cpp";
        if (argc != 1 && !cpp && !(argc == 2 && std::string_view(argv[1]) == "--emit=llvm")) fail("unsupported target");
        std::string source; char c;
        while (std::cin.get(c)) { if (source.size() == 16384) fail("source budget"); source.push_back(c); }
        if (!std::cin.eof() || std::cin.bad()) fail("input read");
        const Compiler compiler(source);
        const auto artifact = cpp ? compiler.emit_cpp() : compiler.emit_llvm();
        std::cout << artifact;
        if (!std::cout) fail("output write");
        return 0;
    } catch (const std::exception& e) { std::cerr << "E_U64_PROFILE: " << e.what() << '\n'; return 2; }
}
