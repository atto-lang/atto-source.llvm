#pragma once
// M2a shared typed pointer HIR: parser, ownership checker and LLVM backend
// consume the SAME nominal, nonescaping Plan. No raw address or integer
// representation of Pointer<int> is exposed as a source-language value.
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace oreslang::pointer_int_aot {

struct Token {
    std::string text;
    std::size_t offset = 0;
    bool number = false;
};

struct Expr {
    enum class Kind { Literal, Variable, Read, Neg, Add, Subtract, Multiply, Length, SizeBytes, ReadonlyCall };
    Kind kind = Kind::Literal;
    std::int64_t known = 0;
    std::size_t index = 0;
    std::string name;
    std::unique_ptr<Expr> lhs;
    std::unique_ptr<Expr> rhs;
};

struct Step {
    enum class Kind { Write, Scalar, Close, Fill, Swap };
    Kind kind = Kind::Close;
    std::string name;
    std::size_t index = 0;
    std::unique_ptr<Expr> value;
    std::size_t second_index = 0; // only Swap may use this field
};

struct Plan {
    enum class Form { OwnedRegion, BorrowedLocal };
    Form form = Form::OwnedRegion;
    // A borrowed local has separate linear handle identity and never
    // obtains the rights of an owning Pointer.alloc region.
    std::string local_handle;
    std::int64_t local_initial = 0;
    // A private helper has one proven readonly singleton read as its entire body.
    std::string readonly_helper;
    std::string readonly_parameter;
    std::string owner;
    std::size_t count = 0;
    std::vector<Step> steps;
    std::unique_ptr<Expr> result;
    // Published exclusively by the shared ownership verifier. The backend
    // refuses all unadmitted plans, including hand-built compiler ASTs.
    bool ownership_admitted = false;
};


} // namespace oreslang::pointer_int_aot
