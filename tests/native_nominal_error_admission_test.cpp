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
    forged_classes[0].verified_stdlib_root = false;
    require(ErrorOperandVerdict::UnverifiedCanonicalRoot,
            nominal("std/error.Error"), forged_classes);
}
