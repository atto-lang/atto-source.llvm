#include "oreslang/frontend/NativeTextClassImport.hpp"

#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace oreslang {
namespace {

struct Token {
    enum class Kind { Word, Number, String, Punctuation, End } kind;
    std::string value;
    std::size_t offset;
};

class Lexer {
public:
    explicit Lexer(std::string_view source) : source_(source) {}
    Token next() {
        while (at_ < source_.size()) {
            const auto c = static_cast<unsigned char>(source_[at_]);
            if (std::isspace(c)) { ++at_; continue; }
            if (source_.substr(at_, 2) == "//") {
                auto end = source_.find('\n', at_ + 2);
                at_ = end == std::string_view::npos ? source_.size() : end;
                continue;
            }
            if (source_.substr(at_, 2) == "/*") {
                at_ += 2;
                std::size_t nesting = 1;
                while (nesting) {
                    if (at_ >= source_.size()) throw std::runtime_error("unterminated block comment");
                    if (source_.substr(at_, 2) == "/*") { ++nesting; at_ += 2; }
                    else if (source_.substr(at_, 2) == "*/") { --nesting; at_ += 2; }
                    else ++at_;
                    if (nesting > 32) throw std::runtime_error("block comment nesting exceeds limit");
                }
                continue;
            }
            break;
        }
        if (at_ == source_.size()) return {Token::Kind::End, "", at_};
        const auto start = at_;
        const auto c = static_cast<unsigned char>(source_[at_++]);
        if (std::isalpha(c) || c == '_') {
            while (at_ < source_.size()) {
                const auto next = static_cast<unsigned char>(source_[at_]);
                if (!std::isalnum(next) && next != '_') break;
                ++at_;
            }
            return {Token::Kind::Word, std::string(source_.substr(start, at_ - start)), start};
        }
        if (std::isdigit(c)) {
            while (at_ < source_.size() && std::isdigit(static_cast<unsigned char>(source_[at_]))) ++at_;
            return {Token::Kind::Number, std::string(source_.substr(start, at_ - start)), start};
        }
        if (c == '"') {
            std::string value;
            bool terminated = false;
            while (at_ < source_.size()) {
                const char next = source_[at_++];
                if (next == '"') { terminated = true; break; }
                if (next == '\n' || next == '\r' || next == '\0')
                    throw std::runtime_error("invalid newline or NUL in string literal");
                if (next == '\\') {
                    if (at_ == source_.size()) throw std::runtime_error("unterminated string escape");
                    switch (source_[at_++]) {
                        case 'n': value += '\n'; break;
                        case 'r': value += '\r'; break;
                        case 't': value += '\t'; break;
                        case '\\': value += '\\'; break;
                        case '"': value += '"'; break;
                        default: throw std::runtime_error("unsupported string escape in native text slice");
                    }
                } else value += next;
                if (value.size() > 65536) throw std::runtime_error("string literal exceeds 64 KiB");
            }
            if (!terminated) throw std::runtime_error("unterminated string literal");
            return {Token::Kind::String, std::move(value), start};
        }
        if (std::string_view("(){},:;+=.").find(static_cast<char>(c)) != std::string_view::npos)
            return {Token::Kind::Punctuation, std::string(1, static_cast<char>(c)), start};
        throw std::runtime_error("unsupported native text token at byte " + std::to_string(start));
    }
private:
    std::string_view source_;
    std::size_t at_ = 0;
};

struct Expr {
    enum class Kind { Literal, Variable, Method, Concat } kind;
    std::string name, receiver;
    std::unique_ptr<Expr> lhs, rhs;
};
struct Method {
    std::string name;
    bool exported = false;
    std::unique_ptr<Expr> result;
};
struct Action {
    enum class Kind { Create, Bind, Print } kind;
    std::string name;
    std::unique_ptr<Expr> value;
    std::vector<std::unique_ptr<Expr>> arguments;
};
struct Unit {
    std::string class_name;
    std::vector<Method> methods;
    std::vector<Action> actions;
    int exit_status = 0;
};

class Parser {
public:
    explicit Parser(std::string_view source) : lexer_(source), token_(lexer_.next()) {}
    std::pair<std::string, std::string> import() {
        expect("import"); expect("class");
        auto name = word();
        expect("from");
        if (token_.kind != Token::Kind::String) fail("expected quoted import file");
        auto relative = token_.value;
        advance(); expect(";");
        return {name, relative};
    }
    void imported_class(Unit& unit, const std::string& expected) {
        // Support both canonical define pub and legacy pub define spellings.
        const bool before = accept("pub");
        expect("define");
        const bool after = accept("pub");
        if (!before && !after) fail("imported class is not exported");
        expect("class");
        unit.class_name = word();
        if (unit.class_name != expected) fail("imported class name does not match selection");
        expect("as");
        std::unordered_set<std::string> names;
        while (!accept("end")) {
            if (token_.kind == Token::Kind::End) fail("unterminated imported class");
            if (unit.methods.size() >= 128) fail("too many class methods");
            Method m;
            m.exported = accept("pub");
            m.name = word();
            if (!names.insert(m.name).second) fail("duplicate method name");
            expect("("); expect(")"); expect(":"); expect("string");
            expect("{"); expect("return"); m.result = expression(); expect(";"); expect("}");
            unit.methods.push_back(std::move(m));
        }
        require_end();
    }
    void entry(Unit& unit) {
        const bool exported = accept("pub");
        if (!exported) fail("main must be public");
        if (!accept("routine")) expect("fnc");
        expect("main"); expect("("); expect(")"); expect(":"); expect("int");
        expect("{");
        std::unordered_set<std::string> locals;
        bool returned = false;
        while (!accept("}")) {
            if (token_.kind == Token::Kind::End) fail("unterminated main");
            if (returned) fail("statements after return are not supported");
            if (accept("val")) {
                auto type = word();
                auto name = word();
                if (!locals.insert(name).second) fail("duplicate local name");
                Action a;
                a.name = name;
                expect("=");
                if (type == unit.class_name) {
                    a.kind = Action::Kind::Create;
                    expect("new"); expect(unit.class_name); expect("("); expect(")");
                } else if (type == "string") {
                    a.kind = Action::Kind::Bind;
                    a.value = expression();
                } else fail("unsupported native text local type");
                expect(";");
                unit.actions.push_back(std::move(a));
            } else if (accept("stdio")) {
                expect("."); expect("println"); expect("(");
                Action a; a.kind = Action::Kind::Print;
                if (!accept(")")) {
                    do { a.arguments.push_back(expression()); } while (accept(","));
                    expect(")");
                }
                expect(";");
                unit.actions.push_back(std::move(a));
            } else if (accept("return")) {
                if (token_.kind != Token::Kind::Number) fail("native text main requires integer return");
                const auto parsed = std::stoull(token_.value);
                if (parsed > 255) fail("main exit status must be 0..255");
                unit.exit_status = static_cast<int>(parsed);
                advance(); expect(";");
                returned = true;
            } else fail("unsupported native text statement");
            if (unit.actions.size() > 2048) fail("too many native text statements");
        }
        if (!returned) fail("main must return int");
        require_end();
    }
    void require_end() const {
        if (token_.kind != Token::Kind::End) fail("trailing source not admitted by native text slice");
    }
private:
    std::unique_ptr<Expr> atom(std::size_t depth) {
        if (depth > 32) fail("string expression nesting exceeds limit");
        auto node = std::make_unique<Expr>();
        if (token_.kind == Token::Kind::String) {
            node->kind = Expr::Kind::Literal;
            node->name = token_.value;
            advance();
        } else {
            auto receiver = word();
            if (accept(".")) {
                node->kind = Expr::Kind::Method;
                node->receiver = receiver;
                node->name = word();
                expect("("); expect(")");
            } else {
                node->kind = Expr::Kind::Variable;
                node->name = receiver;
            }
        }
        return node;
    }
    std::unique_ptr<Expr> expression(std::size_t depth = 0) {
        auto left = atom(depth + 1);
        while (accept("+")) {
            auto joined = std::make_unique<Expr>();
            joined->kind = Expr::Kind::Concat;
            joined->lhs = std::move(left);
            joined->rhs = atom(depth + 1);
            left = std::move(joined);
            if (++depth > 32) fail("string concatenation nesting exceeds limit");
        }
        return left;
    }
    std::string word() {
        if (token_.kind != Token::Kind::Word) fail("expected identifier");
        auto result = token_.value;
        advance();
        return result;
    }
    void advance() { token_ = lexer_.next(); }
    bool accept(std::string_view value) {
        if (token_.value != value) return false;
        advance(); return true;
    }
    void expect(std::string_view value) {
        if (!accept(value)) fail("expected '" + std::string(value) + "'");
    }
    [[noreturn]] void fail(const std::string& why) const {
        throw std::runtime_error("native text/class slice: " + why + " at byte " +
                                 std::to_string(token_.offset));
    }
    Lexer lexer_;
    Token token_;
};

std::string read_import(const std::filesystem::path& entry, const std::string& name) {
    const std::filesystem::path relative(name);
    // First stage admits one literal, sibling .ores import; traversal and
    // additional dependency graphs await the shared cross-unit linker.
    if (relative.parent_path() != "." || relative.extension() != ".ores" ||
        relative.filename() == "." || relative.filename() == "..")
        throw std::runtime_error("native class import requires a sibling .ores path");
    const auto file = entry.parent_path() / relative;
    if (!std::filesystem::is_regular_file(file) || std::filesystem::is_symlink(file))
        throw std::runtime_error("native class import must be a regular, non-symlink file");
    const auto bytes = std::filesystem::file_size(file);
    if (bytes > 1024 * 1024) throw std::runtime_error("native imported source exceeds 1 MiB");
    std::ifstream input(file, std::ios::binary);
    if (!input) throw std::runtime_error("cannot read native class import");
    std::string source((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (source.size() != bytes) throw std::runtime_error("native class import changed during read");
    if (source.find('\0') != std::string::npos) throw std::runtime_error("NUL in imported source");
    return source;
}

void validate_expr(const Expr& expr, const Unit& unit,
                   const std::unordered_map<std::string, bool>& locals,
                   bool inside_method, std::size_t depth = 0) {
    if (depth > 32) throw std::runtime_error("native text expression exceeds depth bound");
    if (expr.kind == Expr::Kind::Literal) return;
    if (expr.kind == Expr::Kind::Concat) {
        validate_expr(*expr.lhs, unit, locals, inside_method, depth + 1);
        validate_expr(*expr.rhs, unit, locals, inside_method, depth + 1);
        return;
    }
    if (expr.kind == Expr::Kind::Variable) {
        const auto found = locals.find(expr.name);
        if (found == locals.end() || !found->second)
            throw std::runtime_error("undefined or non-string native text binding: " + expr.name);
        return;
    }
    if (expr.receiver != "self") {
        if (inside_method) throw std::runtime_error("class method may call only self in first native slice");
        const auto found = locals.find(expr.receiver);
        if (found == locals.end() || found->second)
            throw std::runtime_error("unknown or non-class receiver: " + expr.receiver);
    } else if (!inside_method) {
        throw std::runtime_error("self unavailable in main");
    }
    for (const auto& method : unit.methods) {
        if (method.name != expr.name) continue;
        if (!inside_method && !method.exported)
            throw std::runtime_error("method is private: " + expr.name);
        return;
    }
    throw std::runtime_error("unknown native class method: " + expr.name);
}

void validate(const Unit& unit) {
    const std::unordered_map<std::string, bool> empty;
    for (const auto& method : unit.methods)
        validate_expr(*method.result, unit, empty, true);
    std::unordered_map<std::string, bool> locals;
    for (const auto& action : unit.actions) {
        if (action.kind == Action::Kind::Create) {
            locals.emplace(action.name, false);
        } else if (action.kind == Action::Kind::Bind) {
            validate_expr(*action.value, unit, locals, false);
            locals.emplace(action.name, true);
        } else {
            for (const auto& arg : action.arguments) validate_expr(*arg, unit, locals, false);
        }
    }
}

class Emit {
public:
    explicit Emit(const Unit& unit) : unit_(unit) {}
    std::string result() {
        for (const auto& method : unit_.methods) {
            std::ostringstream fn;
            current_ = &fn;
            next_ = 0;
            fn << "define ptr @\\"ores." << unit_.class_name << "." << method.name
               << "\\"(ptr %self) {\nentry:\n";
            auto value = expression(*method.result, "%self");
            fn << "  ret ptr " << value << "\n}\n";
            functions_ += fn.str();
        }
        {
            std::ostringstream fn;
            current_ = &fn;
            next_ = 0;
            fn << "define i32 @main() {\nentry:\n";
            std::unordered_map<std::string, std::string> objects;
            std::vector<std::string> owned;
            for (const auto& action : unit_.actions) {
                if (action.kind == Action::Kind::Create) {
                    auto instance = reg();
                    fn << "  " << instance << " = alloca i8\n";
                    objects.emplace(action.name, instance);
                } else if (action.kind == Action::Kind::Bind) {
                    const auto value = expression(*action.value, "", objects);
                    values_.emplace(action.name, value);
                    owned.push_back(value);
                } else {
                    for (const auto& arg : action.arguments) {
                        auto value = expression(*arg, "", objects);
                        auto status = reg();
                        fn << "  " << status << " = call i32 (ptr, ...) @printf(ptr "
                           << literal("%s") << ", ptr " << value << ")\n";
                        fn << "  call void @free(ptr " << value << ")\n";
                    }
                    fn << "  call i32 @putchar(i32 10)\n";
                }
            }
            for (auto it = owned.rbegin(); it != owned.rend(); ++it)
                fn << "  call void @free(ptr " << *it << ")\n";
            fn << "  ret i32 " << unit_.exit_status << "\n}\n";
            functions_ += fn.str();
        }
        std::ostringstream ir;
        ir << "; Oreslang checked, host-only native text/class slice\n"
              "source_filename = \\"oreslang-native-text\\"\n"
              "declare ptr @strdup(ptr)\n"
              "declare i32 @asprintf(ptr, ptr, ...)\n"
              "declare i32 @printf(ptr, ...)\n"
              "declare i32 @putchar(i32)\n"
              "declare void @free(ptr)\n"
              "declare void @abort() noreturn\n";
        for (std::size_t i = 0; i < strings_.size(); ++i) {
            const auto& s = strings_[i];
            ir << "@.ore.str." << i << " = private unnamed_addr constant ["
               << s.size() + 1 << " x i8] c\\"";
            static const char hex[] = "0123456789ABCDEF";
            for (unsigned char b : s) {
                if (b >= 32 && b <= 126 && b != '"' && b != '\\')
                    ir << static_cast<char>(b);
                else ir << "\\" << hex[b >> 4] << hex[b & 15];
            }
            ir << "\\00\\"\n";
        }
        ir << functions_;
        return ir.str();
    }
private:
    std::string reg() { return "%ore" + std::to_string(next_++); }
    std::string literal(const std::string& s) {
        const auto id = strings_.size();
        strings_.push_back(s);
        return "getelementptr inbounds ([" + std::to_string(s.size() + 1) +
            " x i8], ptr @.ore.str." + std::to_string(id) + ", i64 0, i64 0)";
    }
    std::string duplicate(const std::string& src) {
        auto value = reg();
        *current_ << "  " << value << " = call ptr @strdup(ptr " << src << ")\n";
        nonnull(value);
        return value;
    }
    void nonnull(const std::string& ptr) {
        auto ok = reg();
        auto label = std::to_string(next_++);
        *current_ << "  " << ok << " = icmp ne ptr " << ptr << ", null\n"
                  << "  br i1 " << ok << ", label %ore.ok." << label
                  << ", label %ore.oom." << label << "\n"
                  << "ore.oom." << label << ":\n"
                  << "  call void @abort()\n"
                  << "  unreachable\n"
                  << "ore.ok." << label << ":\n";
    }
    std::string expression(const Expr& expr, const std::string& receiver,
        const std::unordered_map<std::string, std::string>& objects = {}) {
        if (expr.kind == Expr::Kind::Literal) return duplicate(literal(expr.name));
        if (expr.kind == Expr::Kind::Variable) return duplicate(values_.at(expr.name));
        if (expr.kind == Expr::Kind::Method) {
            auto instance = expr.receiver == "self" ? receiver : objects.at(expr.receiver);
            auto value = reg();
            *current_ << "  " << value << " = call ptr @\\"ores." << unit_.class_name
                      << "." << expr.name << "\\"(ptr " << instance << ")\n";
            nonnull(value);
            return value;
        }
        auto lhs = expression(*expr.lhs, receiver, objects);
        auto rhs = expression(*expr.rhs, receiver, objects);
        auto output = reg();
        auto code = reg();
        auto value = reg();
        *current_ << "  " << output << " = alloca ptr\n"
                  << "  " << code << " = call i32 (ptr, ptr, ...) @asprintf(ptr "
                  << output << ", ptr " << literal("%s%s") << ", ptr "
                  << lhs << ", ptr " << rhs << ")\n"
                  << "  " << value << " = load ptr, ptr " << output << "\n"
                  << "  call void @free(ptr " << lhs << ")\n"
                  << "  call void @free(ptr " << rhs << ")\n";
        auto failed = reg();
        auto label = std::to_string(next_++);
        *current_ << "  " << failed << " = icmp slt i32 " << code << ", 0\n"
                  << "  br i1 " << failed << ", label %ore.oom." << label
                  << ", label %ore.ok." << label << "\n"
                  << "ore.oom." << label << ":\n"
                  << "  call void @abort()\n"
                  << "  unreachable\n"
                  << "ore.ok." << label << ":\n";
        return value;
    }
    const Unit& unit_;
    std::vector<std::string> strings_;
    std::string functions_;
    std::ostringstream* current_ = nullptr;
    std::size_t next_ = 0;
    std::unordered_map<std::string, std::string> values_;
};

} // namespace

bool is_native_text_class_import(std::string_view source) {
    try {
        Lexer lexer(source);
        return lexer.next().value == "import";
    } catch (...) {
        // The normal frontend will issue its own unsupported-token diagnostic.
        return false;
    }
}

std::string emit_native_text_class_import(const std::filesystem::path& entry,
                                          std::string_view source) {
    Parser root(source);
    const auto [class_name, relative] = root.import();
    Unit unit;
    Parser dependency(read_import(entry, relative));
    dependency.imported_class(unit, class_name);
    root.entry(unit);
    validate(unit);
    return Emit(unit).result();
}

} // namespace oreslang
