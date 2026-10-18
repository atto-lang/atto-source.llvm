#pragma once

// HOST-ONLY reference policy for enum-type/case overloads. The actual Oreslang
// LLVM parser, checker, guest codegen and virtual calls MUST still reject this
// source feature until type-identity-preserving lowering has been verified.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace oreslang::types::variant_overloads {
inline constexpr std::size_t MaxArity = 64;
inline constexpr std::size_t MaxSameArity = 128;
inline constexpr std::size_t MaxCases = 1024;
inline constexpr std::size_t MaxFamily = (MaxArity + 1) * MaxSameArity;

enum class Payload { Nullary, Primitive, PrimitiveTuple, Complex };
enum class Kind { Primitive, Enum, Variant };
enum class Domain { Module, Instance, Static };

struct EnumInfo {
    std::string symbol;
    std::map<std::string, Payload> cases;
};
struct Parameter {
    Kind kind;
    std::string id;
    std::string variant;
    bool operator==(const Parameter& other) const {
        return kind == other.kind && id == other.id && variant == other.variant;
    }
};
struct Signature {
    std::string owner;
    Domain domain;
    std::string name;
    std::vector<Parameter> parameters;
    std::size_t arity() const { return parameters.size(); }
};

inline void need(bool condition, const char* what) {
    if (!condition) throw std::invalid_argument(what);
}
inline void qualified(const std::string& value) {
    const std::size_t separator = value.find("::");
    need(!value.empty() && value.size() <= 512 && separator != std::string::npos
         && separator != 0 && separator + 2 < value.size(), "unqualified ID");
    // Native reference deliberately accepts ASCII declarations only until the
    // shared UTF-8 lexical-ID/normalization contract is integrated.
    for (const unsigned char c : value) {
        need(c >= 0x21 && c <= 0x7e, "unsupported native symbol byte");
    }
}
inline void simple(const std::string& name) {
    need(!name.empty() && name.size() <= 512, "invalid symbol name");
    for (const unsigned char c : name) {
        need(c >= 0x21 && c <= 0x7e, "unsupported native symbol byte");
    }
}
inline bool primitive(const std::string& name) {
    static const std::set<std::string> ids = {"bool", "int", "float", "string", "byte", "char",
        "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "f32", "f64"};
    return ids.count(name) != 0;
}
using Registry = std::map<std::string, EnumInfo>;

inline void validateRegistry(const Registry& registry) {
    for (const auto& entry : registry) {
        qualified(entry.first);
        need(entry.second.symbol == entry.first, "forged nominal enum descriptor");
        need(!entry.second.cases.empty() && entry.second.cases.size() <= MaxCases, "enum case limit");
        for (const auto& item : entry.second.cases) {
            simple(item.first);
            switch (item.second) {
                case Payload::Nullary:
                case Payload::Primitive:
                case Payload::PrimitiveTuple:
                case Payload::Complex: break;
                default: throw std::invalid_argument("unknown enum payload layout");
            }
        }
    }
}
inline void validate(const Parameter& p, const Registry& registry) {
    if (p.kind == Kind::Primitive) {
        need(primitive(p.id) && p.variant.empty(), "invalid primitive overload key");
    } else if (p.kind == Kind::Enum || p.kind == Kind::Variant) {
        const auto it = registry.find(p.id);
        need(it != registry.end(), "unknown enum declaration identity");
        for (const auto& member : it->second.cases) {
            need(member.second != Payload::Complex, "complex enum payload not overload-eligible");
        }
        if (p.kind == Kind::Enum) {
            need(p.variant.empty(), "whole enum cannot have case selector");
        } else {
            need(!p.variant.empty() && it->second.cases.count(p.variant) != 0,
                 "unknown enum case declaration identity");
        }
    } else {
        throw std::invalid_argument("unknown kind");
    }
}
inline bool subset(const Parameter& actual, const Parameter& formal) {
    if (actual == formal) return true;
    return actual.kind == Kind::Variant && formal.kind == Kind::Enum && actual.id == formal.id;
}
inline bool subset(const Signature& a, const Signature& b) {
    if (a.arity() != b.arity()) return false;
    for (std::size_t i = 0; i < a.arity(); ++i) {
        if (!subset(a.parameters[i], b.parameters[i])) return false;
    }
    return true;
}
inline bool safePair(const Signature& a, const Signature& b) {
    bool different = false;
    bool disjoint = false;
    bool leftOnly = false;
    bool rightOnly = false;
    for (std::size_t i = 0; i < a.arity(); ++i) {
        const auto& x = a.parameters[i];
        const auto& y = b.parameters[i];
        if (x == y) continue;
        if (x.kind == Kind::Primitive || y.kind == Kind::Primitive) return false;
        different = true;
        if (x.id != y.id) {
            disjoint = true;
        } else if (x.kind == Kind::Enum && y.kind == Kind::Variant) {
            rightOnly = true;
        } else if (x.kind == Kind::Variant && y.kind == Kind::Enum) {
            leftOnly = true;
        } else {
            disjoint = true;
        }
    }
    return different && (disjoint || !(leftOnly && rightOnly));
}

inline void admit(const Registry& registry, const std::vector<Signature>& signatures) {
    need(!signatures.empty() && signatures.size() <= MaxFamily, "callable family limit");
    validateRegistry(registry);
    std::map<std::size_t, std::vector<const Signature*>> groups;
    const Signature& first = signatures.front();
    qualified(first.owner);
    simple(first.name);
    need(first.domain == Domain::Module || first.domain == Domain::Instance
         || first.domain == Domain::Static, "invalid callable domain");
    for (const Signature& s : signatures) {
        need(s.owner == first.owner && s.domain == first.domain && s.name == first.name,
             "mixed callable family");
        need(s.arity() <= MaxArity, "arity limit");
        auto& group = groups[s.arity()];
        need(group.size() < MaxSameArity, "same-arity overload candidate limit");
        group.push_back(&s);
        for (const Parameter& p : s.parameters) validate(p, registry);
    }
    for (const auto& group : groups) {
        const auto& slots = group.second;
        for (std::size_t i = 0; i < slots.size(); ++i) {
            for (std::size_t j = i + 1; j < slots.size(); ++j) {
                need(safePair(*slots[i], *slots[j]), "overlapping/crossing same-arity overloads");
            }
        }
    }
}

// Caller supplies compiler-proven argument keys, NEVER runtime enum tags.
// Returns an index into the declaration vector for AST/codegen binding.
inline std::size_t select(const Registry& registry, const std::vector<Signature>& signatures,
                          const std::vector<Parameter>& staticallyProvenArguments) {
    admit(registry, signatures);
    need(staticallyProvenArguments.size() <= MaxArity, "arity limit");
    for (const auto& p : staticallyProvenArguments) validate(p, registry);
    std::vector<std::size_t> matches;
    for (std::size_t i = 0; i < signatures.size(); ++i) {
        const auto& candidate = signatures[i];
        if (candidate.arity() != staticallyProvenArguments.size()) continue;
        bool applicable = true;
        for (std::size_t j = 0; j < candidate.arity(); ++j) {
            if (!subset(staticallyProvenArguments[j], candidate.parameters[j])) {
                applicable = false;
                break;
            }
        }
        if (applicable) matches.push_back(i);
    }
    need(!matches.empty(), "no statically applicable overload");
    std::size_t winningIndex = signatures.size();
    for (const std::size_t i : matches) {
        bool dominates = true;
        for (const std::size_t j : matches) {
            if (!subset(signatures[i], signatures[j])) { dominates = false; break; }
        }
        if (dominates) {
            need(winningIndex == signatures.size(), "ambiguous overload");
            winningIndex = i;
        }
    }
    need(winningIndex < signatures.size(), "ambiguous overload");
    return winningIndex;
}

inline void field(std::string& output, const std::string& value) {
    constexpr char digits[] = "0123456789abcdef";
    output += '|' + std::to_string(value.size()) + ':';
    for (const unsigned char c : value) {
        output += digits[(c >> 4) & 15];
        output += digits[c & 15];
    }
}
inline std::string symbol(const Signature& s) {
    std::string output = "ores-enum-variant-slot-v1";
    field(output, s.owner);
    field(output, s.domain == Domain::Module ? "MODULE" : s.domain == Domain::Instance ? "INSTANCE" : "STATIC");
    field(output, s.name);
    field(output, std::to_string(s.arity()));
    for (const auto& p : s.parameters) {
        field(output, p.kind == Kind::Primitive ? "P" : p.kind == Kind::Enum ? "E" : "V");
        field(output, p.id);
        if (p.kind == Kind::Variant) field(output, p.variant);
    }
    return output;
}

/**
 * Host-only closed-world AOT link proof.
 *
 * Every source declaration must have a compiled body with a canonical SHA-256
 * digest. The caller's statically proven argument tags yield exactly one
 * relocatable *symbol*, never a runtime enum-switch or a name/arity fallback.
 * The in-memory generation is NOT serialized into the executable ABI.
 */
inline bool canonicalSha256(const std::string& value) {
    if (value.size() != 64) return false;
    for (const char c : value) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

inline std::string validatedSymbol(const Registry& registry, const Signature& signature) {
    qualified(signature.owner);
    simple(signature.name);
    need(signature.arity() <= MaxArity, "AOT symbol arity limit");
    need(signature.domain == Domain::Module || signature.domain == Domain::Instance
         || signature.domain == Domain::Static, "invalid AOT callable domain");
    validateRegistry(registry);
    for (const Parameter& parameter : signature.parameters) validate(parameter, registry);
    return symbol(signature);
}

struct DirectRelocation {
    std::string targetSymbol;
    std::string exactBodyDigest;
    std::uint64_t imageIdentity;
};

class AotImage final {
public:
    AotImage(Registry registry, std::vector<Signature> declarations,
             std::map<std::string, std::string> compiledBodies)
        : registry_(std::move(registry)), declarations_(std::move(declarations)),
          compiledBodies_(std::move(compiledBodies)), imageIdentity_(nextIdentity()) {
        admit(registry_, declarations_);
        need(compiledBodies_.size() == declarations_.size(), "missing or extra AOT definitions");
        for (std::size_t i = 0; i < declarations_.size(); ++i) {
            const Signature& declaration = declarations_[i];
            const std::string token = validatedSymbol(registry_, declaration);
            auto found = compiledBodies_.find(token);
            need(found != compiledBodies_.end(), "unresolved AOT definition");
            need(canonicalSha256(found->second), "noncanonical AOT body digest");
            byArity_[declaration.arity()].push_back(i);
        }
    }
    AotImage(const AotImage&) = delete;
    AotImage& operator=(const AotImage&) = delete;
    AotImage(AotImage&&) = delete;
    AotImage& operator=(AotImage&&) = delete;

    std::size_t definitionCount() const noexcept { return compiledBodies_.size(); }

    DirectRelocation bindDirect(const std::vector<Parameter>& provenArguments) const {
        need(provenArguments.size() <= MaxArity, "AOT call arity limit");
        for (const Parameter& p : provenArguments) validate(p, registry_);
        const auto group = byArity_.find(provenArguments.size());
        need(group != byArity_.end(), "no AOT overload at this arity");
        std::vector<std::size_t> matches;
        for (std::size_t candidateIndex : group->second) {
            const Signature& candidate = declarations_[candidateIndex];
            bool applies = true;
            for (std::size_t i = 0; i < provenArguments.size(); ++i) {
                if (!subset(provenArguments[i], candidate.parameters[i])) {
                    applies = false;
                    break;
                }
            }
            if (applies) matches.push_back(candidateIndex);
        }
        need(!matches.empty(), "no statically proven AOT call target");
        std::size_t winner = declarations_.size();
        for (std::size_t candidateIndex : matches) {
            bool dominates = true;
            for (std::size_t otherIndex : matches) {
                if (!subset(declarations_[candidateIndex], declarations_[otherIndex])) {
                    dominates = false;
                    break;
                }
            }
            if (dominates) {
                need(winner == declarations_.size(), "ambiguous AOT target");
                winner = candidateIndex;
            }
        }
        need(winner < declarations_.size(), "ambiguous AOT target");
        const std::string token = validatedSymbol(registry_, declarations_[winner]);
        auto entry = compiledBodies_.find(token);
        need(entry != compiledBodies_.end(), "unresolved direct-call relocation");
        return DirectRelocation{token, entry->second, imageIdentity_};
    }

    void requireImage(const DirectRelocation& relocation) const {
        need(relocation.imageIdentity == imageIdentity_, "stale AOT image relocation");
        auto body = compiledBodies_.find(relocation.targetSymbol);
        need(body != compiledBodies_.end()
             && body->second == relocation.exactBodyDigest, "invalid AOT body mapping");
    }

private:
    static std::uint64_t nextIdentity() {
        static std::atomic<std::uint64_t> counter{1};
        const auto id = counter.fetch_add(1, std::memory_order_relaxed);
        need(id != 0, "AOT compilation identity exhaustion");
        return id;
    }
    const Registry registry_;
    const std::vector<Signature> declarations_;
    const std::map<std::string, std::string> compiledBodies_;
    std::map<std::size_t, std::vector<std::size_t>> byArity_;
    const std::uint64_t imageIdentity_;
};

} // namespace oreslang::types::variant_overloads
