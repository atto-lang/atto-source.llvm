#pragma once
// STAGED HOST-ONLY compiler oracle for a proposed enum-case refined overload
// ABI. NOT registered in the guest parser, native codegen or dispatch runtime.
// Its inputs require a trusted nominal-resolution/typechecker pass.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace oreslang::types::case_overloads {

constexpr std::size_t MaxArity = 64;
constexpr std::size_t MaxCandidates = 128;
constexpr std::size_t MaxCases = 1024;
constexpr std::size_t MaxEnums = 4096;

enum class Domain { Module, Instance, Static };
enum class Kind { Fixed, Enum, Case };
enum class Relation { Equal, LeftNarrower, RightNarrower, Disjoint, Invalid };

inline bool fixedScalar(const std::string& name) {
    return name == "bool" || name == "int"
        || name == "string" || name == "float";
}

inline void identifier(const std::string& s) {
    if (s.empty()) throw std::invalid_argument("empty nominal identity");
    // Reject malformed UTF-8, embedded NUL, ASCII control and whitespace.
    // Valid multibyte values are kept as their canonical UTF-8 bytes for ABI.
    for (std::size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c <= 0x20 || c == 0x7f)
            throw std::invalid_argument("control/whitespace in identity");
        if (c < 0x80) { ++i; continue; }
        std::size_t len = 0;
        if (c >= 0xc2 && c <= 0xdf) len = 2;
        else if (c >= 0xe0 && c <= 0xef) len = 3;
        else if (c >= 0xf0 && c <= 0xf4) len = 4;
        else throw std::invalid_argument("invalid UTF-8 leader");
        if (i + len > s.size())
            throw std::invalid_argument("truncated UTF-8 identity");
        for (std::size_t n = 1; n < len; ++n) {
            unsigned char part = static_cast<unsigned char>(s[i + n]);
            if (part < 0x80 || part > 0xbf)
                throw std::invalid_argument("invalid UTF-8 continuation");
        }
        unsigned char second = static_cast<unsigned char>(s[i + 1]);
        if ((c == 0xe0 && second < 0xa0)
            || (c == 0xed && second > 0x9f)
            || (c == 0xf0 && second < 0x90)
            || (c == 0xf4 && second > 0x8f))
            throw std::invalid_argument("overlong/surrogate/out-of-range UTF-8");
        // Match Java Character.isISOControl / isWhitespace on Unicode
        // scalars, without locale dependence or implementation-defined
        // isspace() behavior. NBSP/U+2007/U+202F are NOT Java whitespace.
        std::uint32_t scalar = c & (len == 2 ? 0x1fu : (len == 3 ? 0x0fu : 0x07u));
        for (std::size_t n = 1; n < len; ++n) {
            scalar = (scalar << 6) | (static_cast<unsigned char>(s[i+n]) & 0x3fu);
        }
        if ((scalar >= 0x80 && scalar <= 0x9f) || scalar == 0x1680
            || (scalar >= 0x2000 && scalar <= 0x2006)
            || (scalar >= 0x2008 && scalar <= 0x200a)
            || scalar == 0x2028 || scalar == 0x2029
            || scalar == 0x205f || scalar == 0x3000)
            throw std::invalid_argument("Unicode control/whitespace in identity");
        // Explicit version-independent bidi and invisible-format denylist.
        // Must agree with Java's EnumCaseOverloadPolicy.identifier().
        if (scalar == 0x061c || (scalar >= 0x200b && scalar <= 0x200f)
            || (scalar >= 0x202a && scalar <= 0x202e)
            || scalar == 0x2060 || (scalar >= 0x2066 && scalar <= 0x2069)
            || scalar == 0xfeff)
            throw std::invalid_argument("bidi/invisible control in identity");
        i += len;
    }
}
inline void qualified(const std::string& s) {
    identifier(s);
    if (s.front() == '.' || s.back() == '.'
        || s.find('.') == std::string::npos
        || s.find("..") != std::string::npos)
        throw std::invalid_argument("unqualified nominal identity");
}
inline void appendEncoded(std::string& out, const std::string& text) {
    static constexpr char digits[] = "0123456789abcdef";
    out += std::to_string(text.size()) + "_";
    for (unsigned char b : text) {
        out += digits[b >> 4];
        out += digits[b & 15];
    }
}

struct EnumInfo {
    std::string identity;
    std::vector<std::string> cases;
    bool payloadEligible;
};
struct Parameter {
    Kind kind;
    std::string identity;
    std::string caseName;

    static Parameter fixed(std::string id) {
        return {Kind::Fixed, std::move(id), {}};
    }
    static Parameter whole(std::string id) {
        return {Kind::Enum, std::move(id), {}};
    }
    static Parameter selectedCase(std::string id, std::string name) {
        return {Kind::Case, std::move(id), std::move(name)};
    }
    bool operator==(const Parameter& b) const noexcept {
        return kind == b.kind && identity == b.identity && caseName == b.caseName;
    }
};
struct Signature {
    Domain domain;
    std::string owner;
    std::string name;
    std::vector<Parameter> params;
    std::size_t arity() const noexcept { return params.size(); }

    std::string symbolId() const {
        // Validate all direct-aggregate inputs, not just the factory.
        if (domain != Domain::Module && domain != Domain::Instance
            && domain != Domain::Static) throw std::invalid_argument("domain");
        qualified(owner);
        identifier(name);
        if (params.size() > MaxArity)
            throw std::invalid_argument("arity");
        std::string out = "eos2";
        appendEncoded(out, domain == Domain::Module ? "MODULE"
            : (domain == Domain::Instance ? "INSTANCE" : "STATIC"));
        appendEncoded(out, owner);
        appendEncoded(out, name);
        out += "a" + std::to_string(arity()) + "_";
        for (const Parameter& p : params) {
            if (p.kind == Kind::Fixed) {
                identifier(p.identity);
                if (!p.caseName.empty() || !fixedScalar(p.identity))
                    throw std::invalid_argument("unproven fixed parameter");
                out += "F";
            } else if (p.kind == Kind::Enum || p.kind == Kind::Case) {
                qualified(p.identity);
                if (p.kind == Kind::Enum && !p.caseName.empty())
                    throw std::invalid_argument("whole enum case");
                if (p.kind == Kind::Case) identifier(p.caseName);
                out += (p.kind == Kind::Case ? "C" : "E");
            } else {
                throw std::invalid_argument("unrecognized parameter kind");
            }
            appendEncoded(out, p.identity);
            if (p.kind == Kind::Case) appendEncoded(out, p.caseName);
        }
        return out;
    }
};

using Registry = std::unordered_map<std::string, EnumInfo>;

inline void checkParameter(const Parameter& p, const Registry& registry) {
    if (p.kind == Kind::Fixed) {
        identifier(p.identity);
        if (!p.caseName.empty() || !fixedScalar(p.identity))
            throw std::invalid_argument("unproven fixed parameter");
        return;
    }
    if (p.kind != Kind::Enum && p.kind != Kind::Case)
        throw std::invalid_argument("unknown overload type");
    qualified(p.identity);
    const auto it = registry.find(p.identity);
    if (it == registry.end() || !it->second.payloadEligible)
        throw std::invalid_argument("uncertified enum type");
    if (p.kind == Kind::Enum && !p.caseName.empty())
        throw std::invalid_argument("whole enum with case");
    if (p.kind == Kind::Case) {
        identifier(p.caseName);
        const auto& names = it->second.cases;
        if (std::find(names.begin(), names.end(), p.caseName) == names.end())
            throw std::invalid_argument("uncertified enum case");
    }
}
inline Relation relation(const Signature& left, const Signature& right) {
    if (left.domain != right.domain || left.owner != right.owner
        || left.name != right.name || left.arity() != right.arity())
        throw std::invalid_argument("different overload family");
    bool leftSubset = true;
    bool rightSubset = true;
    bool disjoint = false;
    for (std::size_t i = 0; i < left.arity(); ++i) {
        const Parameter& a = left.params[i];
        const Parameter& b = right.params[i];
        if (a.kind == Kind::Fixed || b.kind == Kind::Fixed) {
            if (!(a == b)) return Relation::Invalid;
            continue;
        }
        if (a.identity != b.identity) { disjoint = true; continue; }
        if (a.kind == Kind::Enum && b.kind == Kind::Case) leftSubset = false;
        else if (a.kind == Kind::Case && b.kind == Kind::Enum) rightSubset = false;
        else if (a.kind == Kind::Case && b.kind == Kind::Case
                 && a.caseName != b.caseName) disjoint = true;
    }
    if (disjoint) return Relation::Disjoint;
    if (leftSubset && rightSubset) return Relation::Equal;
    if (leftSubset) return Relation::LeftNarrower;
    if (rightSubset) return Relation::RightNarrower;
    return Relation::Invalid;
}

inline void checkFamily(const std::vector<Signature>& family,
                        const Registry& registry) {
    if (family.empty() || family.size() > MaxCandidates)
        throw std::invalid_argument("family size");
    if (registry.size() > MaxEnums)
        throw std::invalid_argument("enum registry proof budget exceeded");
    for (const auto& entry : registry) {
        qualified(entry.first);
        const EnumInfo& e = entry.second;
        if (entry.first != e.identity || e.cases.empty() || e.cases.size() > MaxCases)
            throw std::invalid_argument("forged enum registry");
        std::unordered_set<std::string> seen;
        for (const auto& name : e.cases) {
            identifier(name);
            if (!seen.insert(name).second)
                throw std::invalid_argument("duplicate enum case");
        }
    }
    const Signature& first = family.front();
    for (std::size_t i = 0; i < family.size(); ++i) {
        const Signature& s = family[i];
        (void)s.symbolId(); // validates immutable-looking direct aggregates.
        if (s.domain != first.domain || s.owner != first.owner
            || s.name != first.name || s.arity() != first.arity())
            throw std::invalid_argument("mixed family");
        for (const Parameter& p : s.params) checkParameter(p, registry);
        for (std::size_t j = 0; j < i; ++j) {
            Relation r = relation(s, family[j]);
            if (r == Relation::Equal || r == Relation::Invalid)
                throw std::invalid_argument("ambiguous case overloads");
        }
    }
}

inline bool accepts(const Parameter& wanted, const Parameter& proven) {
    if (wanted.kind == Kind::Fixed || proven.kind == Kind::Fixed)
        return wanted == proven;
    if (wanted.identity != proven.identity) return false;
    if (wanted.kind == Kind::Enum) return true;
    return proven.kind == Kind::Case && wanted.caseName == proven.caseName;
}

// Compile-time type proof only: returns declaration INDEX. Not a runtime tag
// selector. An unrefined Foo cannot select a Foo.A specialization.
inline std::size_t selectStatic(const std::vector<Signature>& family,
                                const Registry& registry,
                                const std::vector<Parameter>& staticArgs) {
    checkFamily(family, registry);
    if (staticArgs.size() != family.front().arity())
        throw std::invalid_argument("wrong arity");
    for (const Parameter& arg : staticArgs) checkParameter(arg, registry);
    std::vector<std::size_t> applicable;
    for (std::size_t i = 0; i < family.size(); ++i) {
        bool compatible = true;
        for (std::size_t p = 0; p < staticArgs.size(); ++p)
            if (!accepts(family[i].params[p], staticArgs[p])) {
                compatible = false;
                break;
            }
        if (compatible) applicable.push_back(i);
    }
    if (applicable.empty()) throw std::invalid_argument("no static candidate");
    std::size_t winner = family.size();
    for (std::size_t i : applicable) {
        bool specific = true;
        for (std::size_t j : applicable) {
            if (i != j && relation(family[i], family[j]) != Relation::LeftNarrower) {
                specific = false;
                break;
            }
        }
        if (specific) {
            if (winner != family.size())
                throw std::invalid_argument("ambiguous static proof");
            winner = i;
        }
    }
    if (winner == family.size())
        throw std::invalid_argument("ambiguous static proof");
    return winner;
}

} // namespace oreslang::types::case_overloads
