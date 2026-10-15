#pragma once

#include "oreslang/types/NominalEnumOverloadPolicy.h"

#include <algorithm>
#include <cstddef>
#include <map>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

namespace oreslang::types::overloads::methods {

// Static compiler/linker model ONLY. It must not be used as a dynamic
// multimethod, guest-value switch, or a shortcut around arity-only vtables.
// Source enum declarations, class inheritance and executable native vtables
// are separately staged and MUST fail closed until completely implemented.
enum class Domain { Instance, Static };

struct Slot {
    Domain domain;
    Signature signature;
};

inline void requireValidUtf8(const std::string& value) {
    for (std::size_t i = 0; i < value.size();) {
        unsigned char first = static_cast<unsigned char>(value[i]);
        if (first <= 0x7f) { ++i; continue; }
        unsigned width = 0;
        unsigned codepoint = 0;
        unsigned minimum = 0;
        if ((first & 0xe0U) == 0xc0U) {
            width = 2; codepoint = first & 0x1fU; minimum = 0x80;
        } else if ((first & 0xf0U) == 0xe0U) {
            width = 3; codepoint = first & 0x0fU; minimum = 0x800;
        } else if ((first & 0xf8U) == 0xf0U) {
            width = 4; codepoint = first & 0x07U; minimum = 0x10000;
        } else {
            throw std::invalid_argument("invalid UTF-8 nominal method identity");
        }
        if (i + width > value.size()) {
            throw std::invalid_argument("truncated UTF-8 nominal method identity");
        }
        for (unsigned j = 1; j < width; ++j) {
            unsigned char next = static_cast<unsigned char>(value[i + j]);
            if ((next & 0xc0U) != 0x80U) {
                throw std::invalid_argument("invalid UTF-8 continuation in method identity");
            }
            codepoint = (codepoint << 6U) | (next & 0x3fU);
        }
        if (codepoint < minimum || codepoint > 0x10ffffU
                || (codepoint >= 0xd800U && codepoint <= 0xdfffU)) {
            throw std::invalid_argument("noncanonical UTF-8 nominal method identity");
        }
        i += width;
    }
}

inline void appendPart(std::string& out, const std::string& value) {
    static constexpr char Hex[] = "0123456789abcdef";
    out += std::to_string(value.size()) + "_";
    for (unsigned char c : value) {
        out.push_back(Hex[c >> 4U]);
        out.push_back(Hex[c & 15U]);
    }
    out.push_back('$');
}

inline const char* domainName(Domain domain) {
    switch (domain) {
        case Domain::Instance: return "INSTANCE";
        case Domain::Static: return "STATIC";
    }
    throw std::invalid_argument("invalid nominal method domain");
}

inline const char* positionKindName(Kind kind) {
    switch (kind) {
        case Kind::Primitive: return "PRIMITIVE";
        case Kind::Enum: return "ENUM";
    }
    throw std::invalid_argument("invalid nominal method parameter kind");
}

// Cross-backend encoding matches Java NominalMethodSlots.Slot.linkSymbol.
// An owner ID MUST come from lexical declaration resolution, not runtime data.
// A future compiler will produce that verified owner token from its AST.
inline std::string linkSymbol(const std::string& ownerId, const Slot& slot) {
    if (ownerId.empty() || slot.signature.name.empty()) {
        throw std::invalid_argument("empty nominal method owner/name");
    }
    requireValidUtf8(ownerId);
    requireValidUtf8(slot.signature.name);
    validateSignatureShape(slot.signature);
    std::string out = "$methodoverload$v1$";
    appendPart(out, ownerId);
    appendPart(out, domainName(slot.domain));
    appendPart(out, slot.signature.name);
    out += std::to_string(slot.signature.arity()) + "$";
    for (const auto& parameter : slot.signature.positions) {
        appendPart(out, positionKindName(parameter.kind));
        appendPart(out, parameter.id);
    }
    return out;
}

// A conservative proof oracle for local same-arity class families. This does
// not merge inherited methods, implement overrides, or publish native vtables.
// Every enum ID must be known to the compiler's eligible enum registry.
inline std::vector<Slot> proveLocalSameArityFamilies(
        const std::vector<Slot>& localMethods,
        const std::unordered_set<std::string>& certifiedEligibleEnumIds) {
    using Key = std::tuple<Domain, std::string, std::size_t>;
    std::map<Key, std::vector<Slot>> families;
    for (const auto& slot : localMethods) {
        // Public aggregates can bypass the signature factory.
        validateSignatureShape(slot.signature);
        (void) domainName(slot.domain);
        requireValidUtf8(slot.signature.name);
        families[Key{slot.domain, slot.signature.name, slot.signature.arity()}]
                .push_back(slot);
    }

    std::vector<Slot> result;
    for (const auto& family : families) {
        const auto& candidates = family.second;
        if (candidates.size() < 2) continue;
        if (candidates.size() > MaxSameArityCandidates) {
            throw std::invalid_argument("nominal method family candidate bound exceeded");
        }
        for (const auto& candidate : candidates) {
            for (const auto& parameter : candidate.signature.positions) {
                if (parameter.kind == Kind::Enum &&
                        certifiedEligibleEnumIds.find(parameter.id)
                                == certifiedEligibleEnumIds.end()) {
                    throw std::invalid_argument("uncertified nominal enum method parameter");
                }
            }
        }
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            for (std::size_t j = i + 1; j < candidates.size(); ++j) {
                if (compare(candidates[i].signature, candidates[j].signature)
                        != PairStatus::DistinctEnums) {
                    throw std::invalid_argument(
                            "same-arity method signatures require distinct eligible enums");
                }
            }
        }
        result.insert(result.end(), candidates.begin(), candidates.end());
    }

    // No insertion-order assumption may leak from the proof into vtables.
    std::sort(result.begin(), result.end(), [](const Slot& a, const Slot& b) {
        return linkSymbol("__proof_owner__", a) < linkSymbol("__proof_owner__", b);
    });
    return result;
}

} // namespace oreslang::types::overloads::methods
