#pragma once

// Compiler-owned *type-proof policy* for exceptional operands. Guest source
// cannot set verified_stdlib_root; only the future checked canonical std/error
// resolver may issue that bit. This policy neither parses source nor lowers
// guest exceptions. The current native compiler remains fail-closed.
#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace oreslang {

enum class ExceptionalValueShape {
    Nominal, Union, Primitive, String, Structural, Any, Unknown, Null
};
enum class ExceptionalOwnership { Owned, Borrowed, Shared };

struct ResolvedErrorClass {
    // Compiler-resolved stable nominal identities, never source spellings.
    std::string identity;
    std::string parent_identity;
    bool verified_stdlib_root = false;
};

struct ExceptionalOperandProof {
    ExceptionalValueShape shape = ExceptionalValueShape::Unknown;
    ExceptionalOwnership ownership = ExceptionalOwnership::Owned;
    bool nullable = false;
    std::string class_identity;
    // Every variant of a proven sum must be an owned nominal Error subtype.
    std::vector<ExceptionalOperandProof> variants;
};

enum class ErrorOperandVerdict {
    Admitted,
    NotNominal,
    UnknownType,
    NullOperand,
    BorrowedOperand,
    NullableOperand,
    EmptyUnion,
    TooManyUnionVariants,
    UnresolvedClass,
    DuplicateClass,
    UnverifiedCanonicalRoot,
    InvalidHierarchy,
    CyclicHierarchy,
    HierarchyTooDeep,
    ClassBudgetExceeded
};

inline constexpr std::string_view error_operand_verdict_name(
        ErrorOperandVerdict reason) noexcept {
    switch (reason) {
        case ErrorOperandVerdict::Admitted: return "ADMITTED";
        case ErrorOperandVerdict::NotNominal: return "NOT_NOMINAL_ERROR";
        case ErrorOperandVerdict::UnknownType: return "UNKNOWN_ERROR_TYPE";
        case ErrorOperandVerdict::NullOperand: return "NULL_ERROR_OPERAND";
        case ErrorOperandVerdict::BorrowedOperand: return "NONOWNED_ERROR_OPERAND";
        case ErrorOperandVerdict::NullableOperand: return "NULLABLE_ERROR_OPERAND";
        case ErrorOperandVerdict::EmptyUnion: return "EMPTY_ERROR_UNION";
        case ErrorOperandVerdict::TooManyUnionVariants: return "ERROR_UNION_BUDGET";
        case ErrorOperandVerdict::UnresolvedClass: return "UNRESOLVED_NOMINAL_ERROR";
        case ErrorOperandVerdict::DuplicateClass: return "DUPLICATE_NOMINAL_IDENTITY";
        case ErrorOperandVerdict::UnverifiedCanonicalRoot: return "UNVERIFIED_STD_ERROR_ROOT";
        case ErrorOperandVerdict::InvalidHierarchy: return "INVALID_ERROR_HIERARCHY";
        case ErrorOperandVerdict::CyclicHierarchy: return "CYCLIC_ERROR_HIERARCHY";
        case ErrorOperandVerdict::HierarchyTooDeep: return "ERROR_HIERARCHY_DEPTH";
        case ErrorOperandVerdict::ClassBudgetExceeded: return "ERROR_CLASS_BUDGET";
    }
    return "UNKNOWN_ERROR_ADMISSION";
}

// Do not infer a root from a user-defined class merely spelled "Error".
inline constexpr std::string_view kCanonicalErrorIdentity = "std/error.Error";
inline constexpr std::size_t kMaxErrorClasses = 128;
inline constexpr std::size_t kMaxErrorAncestry = 64;
inline constexpr std::size_t kMaxErrorUnionVariants = 32;

inline ErrorOperandVerdict require_nominal_error_operand(
        const ExceptionalOperandProof& operand,
        const std::vector<ResolvedErrorClass>& resolved_classes) {
    if (resolved_classes.size() > kMaxErrorClasses)
        return ErrorOperandVerdict::ClassBudgetExceeded;

    std::unordered_set<std::string_view> identities;
    for (const auto& cls : resolved_classes) {
        if (cls.identity.empty() || !identities.insert(cls.identity).second)
            return ErrorOperandVerdict::DuplicateClass;
        if (cls.verified_stdlib_root &&
            (cls.identity != kCanonicalErrorIdentity ||
             !cls.parent_identity.empty()))
            return ErrorOperandVerdict::InvalidHierarchy;
    }

    // Bounds apply to the *whole* union tree: depth-only guards admit
    // exponentially many variants otherwise. All entries must have a
    // resolved, unique, compiler-proven nominal ancestry path.
    std::size_t remaining_variants = kMaxErrorUnionVariants;
    const auto check = [&](const auto& self,
                           const ExceptionalOperandProof& node,
                           std::size_t depth) -> ErrorOperandVerdict {
        if (node.ownership != ExceptionalOwnership::Owned)
            return ErrorOperandVerdict::BorrowedOperand;
        if (node.nullable)
            return ErrorOperandVerdict::NullableOperand;
        if (node.shape == ExceptionalValueShape::Null)
            return ErrorOperandVerdict::NullOperand;
        if (node.shape == ExceptionalValueShape::Unknown ||
            node.shape == ExceptionalValueShape::Any)
            return ErrorOperandVerdict::UnknownType;
        if (node.shape == ExceptionalValueShape::Union) {
            if (node.variants.empty()) return ErrorOperandVerdict::EmptyUnion;
            if (depth >= kMaxErrorAncestry)
                return ErrorOperandVerdict::HierarchyTooDeep;
            for (const auto& variant : node.variants) {
                if (remaining_variants == 0)
                    return ErrorOperandVerdict::TooManyUnionVariants;
                --remaining_variants;
                const auto result = self(self, variant, depth + 1);
                if (result != ErrorOperandVerdict::Admitted) return result;
            }
            return ErrorOperandVerdict::Admitted;
        }
        if (node.shape != ExceptionalValueShape::Nominal)
            return ErrorOperandVerdict::NotNominal;
        if (node.class_identity.empty() || !node.variants.empty())
            return ErrorOperandVerdict::UnknownType;

        std::unordered_set<std::string_view> seen;
        std::string_view current = node.class_identity;
        for (std::size_t steps = 0; steps < kMaxErrorAncestry; ++steps) {
            if (!seen.insert(current).second)
                return ErrorOperandVerdict::CyclicHierarchy;
            const ResolvedErrorClass* found = nullptr;
            for (const auto& cls : resolved_classes)
                if (cls.identity == current) { found = &cls; break; }
            if (!found) return ErrorOperandVerdict::UnresolvedClass;
            if (current == kCanonicalErrorIdentity) {
                return found->verified_stdlib_root &&
                       found->parent_identity.empty()
                    ? ErrorOperandVerdict::Admitted
                    : ErrorOperandVerdict::UnverifiedCanonicalRoot;
            }
            if (found->verified_stdlib_root ||
                found->parent_identity.empty())
                return ErrorOperandVerdict::InvalidHierarchy;
            current = found->parent_identity;
        }
        return ErrorOperandVerdict::HierarchyTooDeep;
    };
    return check(check, operand, 0);
}

} // namespace oreslang
