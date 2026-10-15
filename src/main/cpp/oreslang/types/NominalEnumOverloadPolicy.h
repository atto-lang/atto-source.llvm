#pragma once

#include <cstddef>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace oreslang::types::overloads {

inline constexpr std::size_t MaxSameArityCandidates = 128;
inline constexpr std::size_t MaxOverloadArity = 64;

// This is a compiler/linker policy, NOT a guest runtime multimethod.
// Native source overload admission remains disabled until enum lowering and
// exact prebound callable IDs are supported end-to-end.
enum class PayloadLayout { Unit, Primitive, PrimitiveTuple, Unsupported };

struct Variant {
    PayloadLayout layout;
    std::vector<std::string> primitiveFields;
};

inline bool isPrimitive(const std::string& name) {
    return name == "bool" || name == "int" || name == "uint"
        || name == "String" || name == "string" || name == "i8"
        || name == "u8" || name == "i16" || name == "u16"
        || name == "i32" || name == "u32" || name == "i64"
        || name == "u64" || name == "f64";
}

inline bool eligibleEnum(const std::vector<Variant>& variants) {
    if (variants.empty()) return false;
    for (const Variant& value : variants) {
        if (value.layout == PayloadLayout::Unit) {
            if (!value.primitiveFields.empty()) return false;
            continue;
        }
        if ((value.layout != PayloadLayout::Primitive
                && value.layout != PayloadLayout::PrimitiveTuple)
            || value.primitiveFields.empty()
            || (value.layout == PayloadLayout::Primitive
                && value.primitiveFields.size() != 1)) return false;
        for (const auto& field : value.primitiveFields) {
            if (!isPrimitive(field)) return false;
        }
    }
    return true;
}

// Enum IDs must come from the compiler's resolved symbol table, never from a
// runtime value, enum variant name, wire tag, structural record or payload.
enum class Kind { Primitive, Enum };
struct Position {
    Kind kind;
    std::string id;

    bool operator==(const Position& rhs) const noexcept {
        return kind == rhs.kind && id == rhs.id;
    }
};
struct Signature {
    std::string name;
    std::vector<Position> positions;
    std::size_t arity() const noexcept { return positions.size(); }
};

enum class PairStatus { DistinctArity, DistinctEnums, Same, InvalidSameArity };

// Signature is a public aggregate; callers can bypass signature(). Recheck
// bounds and canonical shape before any pairwise quadratic proof work.
// Eligibility of enum IDs still requires the compiler's trusted registry.
inline void validateSignatureShape(const Signature& value) {
    if (value.name.empty())
        throw std::invalid_argument("callable name cannot be empty");
    if (value.arity() > MaxOverloadArity)
        throw std::invalid_argument("nominal enum overload arity bound exceeded");
    for (const Position& position : value.positions) {
        if (position.id.empty())
            throw std::invalid_argument("empty overload position identity");
        if (position.kind == Kind::Primitive) {
            if (!isPrimitive(position.id))
                throw std::invalid_argument("unsupported overload primitive identity");
        } else if (position.kind == Kind::Enum) {
            const std::size_t separator = position.id.find("::");
            if (separator == std::string::npos || separator == 0
                || separator + 2 == position.id.size()
                || position.id.find("::::") != std::string::npos)
                throw std::invalid_argument("unqualified nominal enum identity");
        } else {
            throw std::invalid_argument("unknown overload parameter kind");
        }
    }
}

inline PairStatus compare(const Signature& left, const Signature& right) {
    validateSignatureShape(left);
    validateSignatureShape(right);
    if (left.name != right.name)
        throw std::invalid_argument("comparison requires the same callable name");
    if (left.arity() != right.arity()) return PairStatus::DistinctArity;
    bool disjoint = false;
    for (std::size_t i = 0; i < left.arity(); ++i) {
        const Position& a = left.positions[i];
        const Position& b = right.positions[i];
        if (a == b) continue;
        if (a.kind != Kind::Enum || b.kind != Kind::Enum)
            return PairStatus::InvalidSameArity;
        disjoint = true;
    }
    return disjoint ? PairStatus::DistinctEnums : PairStatus::Same;
}

inline Signature signature(
    std::string name,
    const std::vector<std::pair<Kind, std::string>>& resolvedParameters,
    const std::unordered_set<std::string>& verifiedEligibleEnumIds) {
    if (name.empty()) throw std::invalid_argument("callable name cannot be empty");
    if (resolvedParameters.size() > MaxOverloadArity)
        throw std::invalid_argument("nominal enum overload arity bound exceeded");
    Signature result{std::move(name), {}};
    for (const auto& parameter : resolvedParameters) {
        if (parameter.second.empty()) {
            throw std::invalid_argument("parameter has no statically resolved type ID");
        }
        if (parameter.first != Kind::Primitive && parameter.first != Kind::Enum)
            throw std::invalid_argument("unknown overload parameter kind");
        if (parameter.first == Kind::Primitive && !isPrimitive(parameter.second)) {
            throw std::invalid_argument(
                "structural/borrowed/generic/unknown primitive overload type");
        }
        if (parameter.first == Kind::Enum
            && verifiedEligibleEnumIds.find(parameter.second)
                == verifiedEligibleEnumIds.end()) {
            throw std::invalid_argument(
                "unverified or ineligible nominal enum overload symbol");
        }
        std::string id = parameter.second;
        if (parameter.first == Kind::Primitive && id == "String") id = "string";
        result.positions.push_back(Position{parameter.first, std::move(id)});
    }
    validateSignatureShape(result);
    return result;
}

// Returns an index, not a runtime-selected callable. Caller MUST check all
// arguments against the selected signature after this nominal-only step.
inline std::size_t select(
    const std::vector<Signature>& candidates,
    const std::unordered_map<std::size_t, std::string>& staticEnumProofs) {
    if (candidates.empty()) throw std::invalid_argument("empty overload set");
    if (candidates.size() > MaxSameArityCandidates)
        throw std::invalid_argument("nominal enum overload candidate bound exceeded");
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        validateSignatureShape(candidates[i]);
        if (candidates[i].name != candidates.front().name
            || candidates[i].arity() != candidates.front().arity())
            throw std::invalid_argument("mixed-name/arity overload family");
        for (std::size_t j = 0; j < i; ++j) {
            if (compare(candidates[i], candidates[j]) != PairStatus::DistinctEnums)
                throw std::invalid_argument("ambiguous same-arity overload family");
        }
    }

    std::unordered_set<std::size_t> discriminatorPositions;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        for (std::size_t j = i + 1; j < candidates.size(); ++j) {
            for (std::size_t p = 0; p < candidates[i].arity(); ++p) {
                if (!(candidates[i].positions[p] == candidates[j].positions[p]))
                    discriminatorPositions.insert(p);
            }
        }
    }
    if (staticEnumProofs.size() != discriminatorPositions.size()) {
        throw std::invalid_argument("incomplete static nominal enum overload proof");
    }
    for (const auto& proof : staticEnumProofs) {
        if (discriminatorPositions.find(proof.first) == discriminatorPositions.end()
            || proof.second.empty()) {
            throw std::invalid_argument("invalid or extraneous enum proof position");
        }
    }
    std::size_t chosen = 0;
    std::size_t matches = 0;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        bool match = true;
        for (const auto& proof : staticEnumProofs) {
            if (proof.first >= candidates[i].arity())
                throw std::invalid_argument("invalid nominal discriminator index");
            const Position& key = candidates[i].positions[proof.first];
            if (key.kind != Kind::Enum || key.id != proof.second) {
                match = false;
                break;
            }
        }
        if (match) {
            chosen = i;
            ++matches;
        }
    }
    if (matches != 1)
        throw std::invalid_argument("cannot statically bind a unique enum overload");
    return chosen;
}

} // namespace oreslang::types::overloads
