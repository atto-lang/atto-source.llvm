#include "oreslang/frontend/ClientContractHeader.hpp"

#include <cctype>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace oreslang {
namespace {
const std::unordered_set<std::string> KEYWORDS = {
    "abstract",
    "actor",
    "arr",
    "as",
    "async",
    "await",
    "block",
    "break",
    "catch",
    "cb",
    "class",
    "conforms",
    "const",
    "continue",
    "contract",
    "def",
    "default",
    "defer",
    "define",
    "do",
    "done",
    "elif",
    "else",
    "elseif",
    "end",
    "eq",
    "extends",
    "false",
    "fi",
    "finally",
    "fnc",
    "for",
    "from",
    "generator",
    "if",
    "impl",
    "implements",
    "import",
    "interface",
    "is",
    "isoactor",
    "let",
    "loop",
    "match",
    "matches",
    "module",
    "mut",
    "namespace",
    "nb",
    "neq",
    "new",
    "nlex",
    "null",
    "obj",
    "of",
    "private",
    "pub",
    "readch",
    "return",
    "routine",
    "rt",
    "select",
    "self",
    "shared",
    "spawn",
    "static",
    "stop",
    "struct",
    "super",
    "suspends",
    "switch",
    "then",
    "trait",
    "trap",
    "true",
    "try",
    "type",
    "typeof",
    "types",
    "untrusted",
    "val",
    "void",
    "writech",
    "yield",
    "String", "suspend", "owned", "critical"
};
bool identifier_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
bool identifier_part(char c) {
    return identifier_start(c) || (c >= '0' && c <= '9');
}

class Tokens {
 public:
    explicit Tokens(std::string_view source) : source_(source) {
        if (source.size() > 1048576) {
            throw std::runtime_error("contract header exceeds 1 MiB");
        }
        advance();
    }

    bool at(std::string_view value) const { return token_ == value; }
    bool done() const { return token_.empty(); }
    void need(std::string_view value) {
        if (!at(value)) fail("expected '" + std::string(value) + "'");
        advance();
    }
    std::string name() {
        if (done() || !identifier_start(token_.front()) ||
            KEYWORDS.count(token_) != 0) {
            fail("expected non-reserved identifier");
        }
        const std::string result = token_;
        advance();
        return result;
    }
    [[noreturn]] void fail(const std::string& message) const {
        throw std::runtime_error("Oreslang client contract header at byte " +
                                 std::to_string(offset_) + ": " + message);
    }
    void advance() {
        while (next_ < source_.size()) {
            if (std::isspace(static_cast<unsigned char>(source_[next_])) != 0) {
                ++next_;
            } else if (source_.substr(next_, 2) == "//") {
                const auto e = source_.find('\n', next_ + 2);
                next_ = e == std::string_view::npos ? source_.size() : e;
            } else if (source_.substr(next_, 2) == "/*") {
                const auto e = source_.find("*/", next_ + 2);
                if (e == std::string_view::npos) fail("unclosed block comment");
                next_ = e + 2;
            } else {
                break;
            }
        }
        offset_ = next_;
        if (next_ == source_.size()) { token_.clear(); return; }
        const char ch = source_[next_];
        if (identifier_start(ch)) {
            const auto start = next_++;
            while (next_ < source_.size() && identifier_part(source_[next_])) ++next_;
            token_ = std::string(source_.substr(start, next_ - start));
        } else if (source_.substr(next_, 2) == "=>") {
            token_ = "=>";
            next_ += 2;
        } else if (ch == '(' || ch == ')' || ch == ',' || ch == ';') {
            token_ = std::string(1, ch);
            ++next_;
        } else {
            fail("unsupported token");
        }
    }

 private:
    std::string_view source_;
    std::size_t next_ = 0;
    std::size_t offset_ = 0;
    std::string token_;
};
} // namespace

ClientContractHeader parse_client_contract_header(std::string_view source) {
    Tokens token(source);
    if (token.at("pub")) token.need("pub");
    token.need("define");
    token.need("contract");
    ClientContractHeader result;
    result.name = token.name();
    token.need("as");
    std::unordered_set<std::string> names;
    while (!token.at("end")) {
        if (token.done()) token.fail("missing end");
        token.need("fnc");
        ClientContractMethod method;
        method.name = token.name();
        if (!names.insert(method.name).second) token.fail("duplicate contract method");
        token.need("(");
        std::unordered_set<std::string> params;
        if (!token.at(")")) {
            for (;;) {
                token.need("String");
                std::string param = token.name();
                if (!params.insert(param).second) token.fail("duplicate parameter");
                method.parameters.push_back(std::move(param));
                if (!token.at(",")) break;
                token.need(",");
            }
        }
        token.need(")");
        token.need("=>");
        token.need("String");
        token.need(";");
        result.methods.push_back(std::move(method));
        if (result.methods.size() > 4096) token.fail("too many contract methods");
    }
    token.need("end");
    if (!token.done()) token.fail("unexpected trailing source");
    return result;
}
} // namespace oreslang
