#pragma once

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oreslang {

// Native AOT selector foundation, deliberately independent from the current
// i64-only source AST. A known tag is a *type identity*, never an enum member
// numeric value or structural hash.
struct NominalTypeTag {
    enum class Kind { Primitive, Enum };
    Kind kind;
    std::string identity;

    bool operator==(const NominalTypeTag& other) const {
        return kind == other.kind && identity == other.identity;
    }
    bool operator!=(const NominalTypeTag& other) const {
        return !(*this == other);
    }
};

inline NominalTypeTag primitive_tag(std::string identity) {
    if (identity != "int" && identity != "bool" && identity != "string"
            && identity != "float" && identity != "decimal"
            && identity != "complex")
        throw std::invalid_argument("unrecognized canonical primitive tag");
    return {NominalTypeTag::Kind::Primitive, std::move(identity)};
}

// The supplied identity must be linker-canonical and qualified by source
// unit/module/declaration. Never use the enum's simple spelling alone.
inline NominalTypeTag enum_tag(std::string canonical_identity) {
    if (canonical_identity.empty() || canonical_identity.size() > 1024)
        throw std::invalid_argument("invalid canonical enum identity length");
    for (unsigned char ch : canonical_identity) {
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')
                || (ch >= '0' && ch <= '9') || ch == '_' || ch == '.'
                || ch == '/' || ch == '-' || ch == ':'))
            throw std::invalid_argument("invalid canonical enum identity character");
    }
    const auto boundary = canonical_identity.rfind("::");
    if (boundary == std::string::npos || boundary == 0
            || boundary + 2 >= canonical_identity.size())
        throw std::invalid_argument(
            "enum identity must qualify its declaration with a source owner");
    const auto first = static_cast<unsigned char>(canonical_identity[boundary + 2]);
    if (!((first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z') || first == '_'))
        throw std::invalid_argument("enum declaration name must begin with an identifier character");
    for (std::size_t i = boundary + 3; i < canonical_identity.size(); ++i) {
        const auto ch = static_cast<unsigned char>(canonical_identity[i]);
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')
                || (ch >= '0' && ch <= '9') || ch == '_'))
            throw std::invalid_argument("enum declaration name must be a simple identifier");
    }
    return {NominalTypeTag::Kind::Enum, std::move(canonical_identity)};
}

struct NominalOverloadSlot {
    std::string owner;
    std::string name;
    std::vector<std::optional<NominalTypeTag>> parameter_tags;
};

inline void validate_nominal_overload_family(const std::vector<NominalOverloadSlot>& slots) {
    if (slots.size() > 256) throw std::invalid_argument("nominal overload family exceeds 256 declarations");
    if (slots.empty()) return;
    const auto& first = slots.front();
    if (first.name.empty() || first.parameter_tags.size() > 64)
        throw std::invalid_argument("invalid nominal overload name/arity");
    for (std::size_t i = 0; i < slots.size(); ++i) {
        const auto& a = slots[i];
        if (a.owner != first.owner || a.name != first.name
                || a.parameter_tags.size() != first.parameter_tags.size())
            throw std::invalid_argument("nominal overload family mixes owner/name/arity");
        for (std::size_t j = 0; j < i; ++j) {
            const auto& b = slots[j];
            bool distinguishable = false;
            for (std::size_t k = 0; k < a.parameter_tags.size(); ++k) {
                if (a.parameter_tags[k] && b.parameter_tags[k]
                        && *a.parameter_tags[k] != *b.parameter_tags[k]) {
                    distinguishable = true;
                    break;
                }
            }
            if (!distinguishable)
                throw std::invalid_argument("same-arity nominal overloads overlap; no structural fallback");
        }
    }
}

// Called only after a typed source frontend has resolved argument static
// types. nullopt means unresolved / generic / union, never a runtime lookup.
inline std::size_t resolve_nominal_overload_static(
        const std::vector<NominalOverloadSlot>& slots,
        const std::vector<std::optional<NominalTypeTag>>& argument_tags) {
    validate_nominal_overload_family(slots);
    std::optional<std::size_t> chosen;
    for (std::size_t i = 0; i < slots.size(); ++i) {
        const auto& candidate = slots[i];
        if (candidate.parameter_tags.size() != argument_tags.size()) continue;
        bool matches = true;
        for (std::size_t pos = 0; pos < argument_tags.size(); ++pos) {
            if (candidate.parameter_tags[pos]
                    && (!argument_tags[pos]
                            || *candidate.parameter_tags[pos] != *argument_tags[pos])) {
                matches = false;
                break;
            }
        }
        if (matches) {
            if (chosen) throw std::invalid_argument("ambiguous nominal overload selection");
            chosen = i;
        }
    }
    if (!chosen) throw std::invalid_argument("no matching static nominal overload");
    return *chosen;
}

// A deterministic *internal* key; future linker metadata must bind the same
// canonical fully-qualified enum identities across Java/LLVM/Wasm targets.
// Hex encoding is injective and cannot alias a '$' source identifier.
inline std::string nominal_overload_link_key(const NominalOverloadSlot& slot) {
    // Fully encode the owner, function name, arity and parameter tags.
    // Raw concatenation would permit distinct source identities to alias.
    static constexpr char hex[] = "0123456789abcdef";
    if (slot.owner.empty() || slot.owner.size() > 1024
            || slot.name.empty() || slot.name.size() > 128
            || slot.parameter_tags.size() > 64)
        throw std::invalid_argument("invalid overload scope, name or arity");
    const auto encode = [&](const std::string& input) {
        std::string out = std::to_string(input.size()) + "_";
        for (unsigned char ch : input) {
            out.push_back(hex[ch >> 4]);
            out.push_back(hex[ch & 15]);
        }
        return out;
    };
    std::string key = "$ores$nominal$v1$o" + encode(slot.owner)
            + "$n" + encode(slot.name)
            + "$a" + std::to_string(slot.parameter_tags.size());
    for (const auto& tag : slot.parameter_tags) {
        if (!tag) { key += "$u"; continue; }
        if (tag->identity.empty() || tag->identity.size() > 1024)
            throw std::invalid_argument("invalid overload tag identity length");
        key += tag->kind == NominalTypeTag::Kind::Enum ? "$e" : "$p";
        key += encode(tag->identity);
    }
    return key;
}

} // namespace oreslang
