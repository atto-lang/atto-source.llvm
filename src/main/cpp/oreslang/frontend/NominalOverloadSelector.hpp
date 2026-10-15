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
    // Keep ABI-compatible with the Java compiler's canonical scalar tags.
    // An alias is normalized before publication, never treated as distinct.
    if (identity == "String") identity = "string";
    if (identity != "bool" && identity != "int" && identity != "uint"
            && identity != "string" && identity != "i8" && identity != "u8"
            && identity != "i16" && identity != "u16"
            && identity != "i32" && identity != "u32"
            && identity != "i64" && identity != "u64"
            && identity != "f64")
        throw std::invalid_argument("unrecognized canonical primitive tag");
    return {NominalTypeTag::Kind::Primitive, std::move(identity)};
}

// The supplied identity must be linker-canonical and qualified by source
// unit/module/declaration. Never use the enum's simple spelling alone.
inline NominalTypeTag enum_tag(std::string canonical_identity) {
    if (canonical_identity.empty() || canonical_identity.size() > 1024)
        throw std::invalid_argument("invalid canonical enum identity length");
    // A doubled scope delimiter cannot name a canonical declaration owner.
    // Keep this aligned with Java's nominal overload Position admission.
    if (canonical_identity.find("::::") != std::string::npos)
        throw std::invalid_argument("invalid repeated canonical scope delimiter");
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

// Type tags are plain internal records so that backends may construct them
// from typed IR. Treat these records as UNTRUSTED at every API boundary:
// a caller could bypass primitive_tag()/enum_tag(), or synthesize a bad enum
// discriminant with a cast. Never put such a value into an ABI symbol key.
inline void validate_nominal_tag(const NominalTypeTag& tag) {
    switch (tag.kind) {
        case NominalTypeTag::Kind::Primitive:
            if (primitive_tag(tag.identity).identity != tag.identity)
                throw std::invalid_argument("noncanonical primitive tag identity");
            return;
        case NominalTypeTag::Kind::Enum:
            (void)enum_tag(tag.identity);
            return;
    }
    throw std::invalid_argument("unknown nominal tag discriminant");
}

// Same-arity admission matches Java: ONLY distinct nominal enum IDs can
// discriminate; *every* other position must be the identical known primitive
// or enum identity. Primitive-only or structural/dynamic discrimination is
// forbidden, even if another position would otherwise distinguish the pair.
inline void validate_nominal_overload_family(const std::vector<NominalOverloadSlot>& slots) {
    if (slots.size() > 128)
        throw std::invalid_argument("nominal overload family exceeds 128 declarations");
    if (slots.empty()) return;
    const auto& first = slots.front();
    if (first.owner.empty() || first.owner.size() > 1024
            || first.name.empty() || first.name.size() > 128
            || first.parameter_tags.size() > 64)
        throw std::invalid_argument("invalid nominal overload owner/name/arity");
    for (std::size_t i = 0; i < slots.size(); ++i) {
        const auto& a = slots[i];
        if (a.owner != first.owner || a.name != first.name
                || a.parameter_tags.size() != first.parameter_tags.size())
            throw std::invalid_argument("nominal overload family mixes owner/name/arity");
        for (const auto& tag : a.parameter_tags) {
            if (!tag)
                throw std::invalid_argument("unresolved overload parameter tag");
            validate_nominal_tag(*tag);
        }
        for (std::size_t j = 0; j < i; ++j) {
            const auto& b = slots[j];
            bool distinct_enums = false;
            for (std::size_t k = 0; k < a.parameter_tags.size(); ++k) {
                const auto& left = *a.parameter_tags[k];
                const auto& right = *b.parameter_tags[k];
                if (left == right) continue;
                if (left.kind != NominalTypeTag::Kind::Enum
                        || right.kind != NominalTypeTag::Kind::Enum)
                    throw std::invalid_argument(
                        "primitive or enum/primitive difference cannot distinguish same-arity overloads");
                distinct_enums = true;
            }
            if (!distinct_enums)
                throw std::invalid_argument("same-arity nominal overloads overlap");
        }
    }
}

// Called only after a typed source frontend has resolved argument static
// types. nullopt means unresolved / generic / union, never a runtime lookup.
inline std::size_t resolve_nominal_overload_static(
        const std::vector<NominalOverloadSlot>& slots,
        const std::vector<std::optional<NominalTypeTag>>& argument_tags) {
    validate_nominal_overload_family(slots);
    if (argument_tags.size() > 64)
        throw std::invalid_argument("nominal argument list exceeds 64 values");
    for (const auto& tag : argument_tags)
        if (tag) validate_nominal_tag(*tag);
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
        if (!tag) throw std::invalid_argument("unresolved overload ABI parameter");
        validate_nominal_tag(*tag);
        key += tag->kind == NominalTypeTag::Kind::Enum ? "$e" : "$p";
        key += encode(tag->identity);
    }
    return key;
}

} // namespace oreslang
