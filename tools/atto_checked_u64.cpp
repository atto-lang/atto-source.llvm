// Explicit closed unsigned scalar compiler candidate. Emit only validated LLVM IR.
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
            auto op = peek(); int prec = op == "*" ? 2 : op == "+" || op == "-" ? 1 : 0;
            if (prec < min) break;
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
        else target = n.value == "+" ? "checked_add" : n.value == "-" ? "checked_sub" : "checked_mul";
        auto v = "%v" + std::to_string(next++);
        out += "  " + v + " = call i64 @" + target + "(";
        for (std::size_t i = 0; i < args.size(); ++i) out += (i ? ", i64 " : "i64 ") + args[i];
        out += ")\n"; return v;
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
            else if (std::string_view("(){}:,;+-*").find(c) == std::string_view::npos) fail("unsupported source byte");
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
    std::string emit() const {
        std::string out = "declare void @llvm.trap()\n";
        for (auto op : {"add", "sub"}) {
            std::string s(op), intrinsic = "@llvm.u" + s + ".with.overflow.i64";
            out += "declare { i64, i1 } " + intrinsic + "(i64, i64)\n";
            out += "define internal i64 @checked_" + s + "(i64 %a, i64 %b) {\nentry:\n  %r = call { i64, i1 } " + intrinsic + "(i64 %a, i64 %b)\n  %bad = extractvalue { i64, i1 } %r, 1\n  br i1 %bad, label %overflow, label %ok\noverflow:\n  call void @llvm.trap()\n  unreachable\nok:\n  %v = extractvalue { i64, i1 } %r, 0\n  ret i64 %v\n}\n";
        }
        // Avoid a freestanding wasm32 __multi3 dependency at -O0. The
        // divide-before-multiply guard is unsigned and never divides by zero.
        out += "define internal i64 @checked_mul(i64 %a, i64 %b) {\nentry:\n  %zero = icmp eq i64 %b, 0\n  br i1 %zero, label %ok, label %guard\nguard:\n  %limit = udiv i64 -1, %b\n  %bad = icmp ugt i64 %a, %limit\n  br i1 %bad, label %overflow, label %ok\noverflow:\n  call void @llvm.trap()\n  unreachable\nok:\n  %v = mul i64 %a, %b\n  ret i64 %v\n}\n";
        for (std::size_t i = 0; i < funcs.size(); ++i) {
            const auto& f = funcs[i]; out += "define internal i64 @f" + std::to_string(i) + "(";
            for (std::size_t p = 0; p < f.params.size(); ++p) out += (p ? ", i64 %p" : "i64 %p") + std::to_string(p);
            out += ") {\nentry:\n"; unsigned next = 0; auto v = emit_node(*f.body, f, out, next); out += "  ret i64 " + v + "\n}\n";
        }
        out += "define i64 @atto_guest_main() {\nentry:\n  %v = call i64 @f" + std::to_string(index.at("main")) + "()\n  ret i64 %v\n}\n";
        if (out.size() > 131072) fail("emitted IR budget");
        return out;
    }
};
} // namespace
int main() {
    try {
        std::string source; char c;
        while (std::cin.get(c)) { if (source.size() == 16384) fail("source budget"); source.push_back(c); }
        if (!std::cin.eof() || std::cin.bad()) fail("input read");
        const auto ir = Compiler(source).emit(); std::cout << ir;
        if (!std::cout) fail("output write");
        return 0;
    } catch (const std::exception& e) { std::cerr << "E_U64_PROFILE: " << e.what() << '\n'; return 2; }
}
