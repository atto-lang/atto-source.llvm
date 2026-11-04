#include "oreslang/types/NominalErrorAdmission.hpp"
#include <stdexcept>
#include <string>
#include <vector>

using namespace oreslang;

static ExceptionalOperandProof nominal(const std::string& id) {
    ExceptionalOperandProof p;
    p.shape = ExceptionalValueShape::Nominal;
    p.class_identity = id;
    return p;
}
static void require(ErrorOperandVerdict expected, const ExceptionalOperandProof& operand,
                    const std::vector<ResolvedErrorClass>& classes) {
    if (require_nominal_error_operand(operand, classes) != expected)
        throw std::runtime_error("unexpected canonical Error admission");
}
int main() {
    const std::vector<ResolvedErrorClass> classes = {
        {"std/error.Error", "", true},
        {"pkg.BaseError", "std/error.Error", false},
        {"pkg.MyError", "pkg.BaseError", false},
        {"local.Error", "", false}
    };
    require(ErrorOperandVerdict::Admitted, nominal("std/error.Error"), classes);
    require(ErrorOperandVerdict::Admitted, nominal("pkg.MyError"), classes);
    require(ErrorOperandVerdict::InvalidHierarchy, nominal("local.Error"), classes);
    require(ErrorOperandVerdict::UnresolvedClass, nominal("Unknown"), classes);
    ExceptionalOperandProof primitive;
    primitive.shape = ExceptionalValueShape::Primitive;
    require(ErrorOperandVerdict::NotNominal, primitive, classes);
    auto borrowed = nominal("pkg.MyError");
    borrowed.ownership = ExceptionalOwnership::Borrowed;
    require(ErrorOperandVerdict::BorrowedOperand, borrowed, classes);
    auto nullable = nominal("pkg.MyError");
    nullable.nullable = true;
    require(ErrorOperandVerdict::NullableOperand, nullable, classes);
    auto forged_classes = classes;
    ExceptionalOperandProof alternatives;
    alternatives.shape = ExceptionalValueShape::Union;
    require(ErrorOperandVerdict::EmptyUnion, alternatives, classes);
    alternatives.variants.push_back(nominal("pkg.BaseError"));
    alternatives.variants.push_back(nominal("pkg.MyError"));
    require(ErrorOperandVerdict::Admitted, alternatives, classes);
    alternatives.variants.push_back(primitive);
    require(ErrorOperandVerdict::NotNominal, alternatives, classes);
    alternatives.variants.pop_back();
    for (unsigned i = 0; i < kMaxErrorUnionVariants; ++i)
        alternatives.variants.push_back(nominal("pkg.BaseError"));
    require(ErrorOperandVerdict::TooManyUnionVariants, alternatives, classes);
    auto duplicates = classes;
    duplicates.push_back(classes[2]);
    require(ErrorOperandVerdict::DuplicateClass, nominal("pkg.MyError"), duplicates);
    auto cyclic = classes;
    cyclic.push_back({"pkg.CycleA", "pkg.CycleB", false});
    cyclic.push_back({"pkg.CycleB", "pkg.CycleA", false});
    require(ErrorOperandVerdict::CyclicHierarchy, nominal("pkg.CycleA"), cyclic);
    auto invalid_parent = classes;
    invalid_parent.push_back({"pkg.Orphan", "pkg.Ghost", false});
    require(ErrorOperandVerdict::UnresolvedClass, nominal("pkg.Orphan"), invalid_parent);
    auto forged_nested = classes;
    forged_nested[1].verified_stdlib_root = true;
    require(ErrorOperandVerdict::InvalidHierarchy, nominal("pkg.MyError"), forged_nested);
    auto any_value = primitive;
    any_value.shape = ExceptionalValueShape::Any;
    require(ErrorOperandVerdict::UnknownType, any_value, classes);
    any_value.shape = ExceptionalValueShape::Unknown;
    require(ErrorOperandVerdict::UnknownType, any_value, classes);
    any_value.shape = ExceptionalValueShape::Null;
    require(ErrorOperandVerdict::NullOperand, any_value, classes);
    any_value.shape = ExceptionalValueShape::Structural;
    require(ErrorOperandVerdict::NotNominal, any_value, classes);
    auto shared = nominal("pkg.MyError");
    shared.ownership = ExceptionalOwnership::Shared;
    require(ErrorOperandVerdict::BorrowedOperand, shared, classes);
    std::vector<ResolvedErrorClass> excessive_classes;
    for (std::size_t n = 0; n <= kMaxErrorClasses; ++n)
        excessive_classes.push_back({"pkg.C" + std::to_string(n), "", false});
    require(ErrorOperandVerdict::ClassBudgetExceeded,
            nominal("pkg.C0"), excessive_classes);
    std::vector<ResolvedErrorClass> deep_classes = {
        {"std/error.Error", "", true}
    };
    for (unsigned n = 0; n < kMaxErrorAncestry + 2; ++n)
        deep_classes.push_back({"pkg.Deep" + std::to_string(n),
            n == 0 ? "std/error.Error" : "pkg.Deep" + std::to_string(n - 1), false});
    require(ErrorOperandVerdict::HierarchyTooDeep,
            nominal("pkg.Deep" + std::to_string(kMaxErrorAncestry + 1)),
            deep_classes);
    forged_classes[0].verified_stdlib_root = false;
    require(ErrorOperandVerdict::UnverifiedCanonicalRoot,
            nominal("std/error.Error"), forged_classes);
}
