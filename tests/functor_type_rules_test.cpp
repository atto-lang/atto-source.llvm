#include "oreslang/types/FunctorTypeRules.hpp"
#include "oreslang/types/NativeSumTypes.hpp"

#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace oreslang::types::functor_v1;

namespace {
unsigned passed = 0;

void check(bool condition, const std::string& label) {
    if (!condition) throw std::runtime_error("host-only functor check failed: " + label);
    ++passed;
}
void shape(const Type& actual, const std::string& expected) {
    check(actual == parse_type(expected), "type shape " + actual.str() + " expected " + expected);
}
void reject(const std::string& category, const std::function<void()>& f) {
    try {
        f();
    } catch (const TypeError& e) {
        check(e.category == category, "negative category " + e.category + " expected " + category);
        return;
    }
    throw std::runtime_error("accepted unsafe host-only functor type operation: " + category);
}
} // namespace

int main() {
    // Deterministic nested type parsing, nominal type identity, and shape laws.
    const Type i = parse_type("int");
    const Type opt = parse_type("Option<int>");
    const Type res = parse_type("Result<int,DomainA>");
    shape(parse_type(" Option < Result < Tree < int > , DomainA > > "),
          "Option<Result<Tree<int>,DomainA>>");
    shape(parse_type("F<G<H<int>>>"), "F<G<H<int>>>");
    shape(parse_type("Result<Either<Left,Right>,DomainA>"),
          "Result<Either<Left,Right>,DomainA>");
    shape(parse_type("Result<int,string>"), "Result<int,String>");
    check(parse_type("Result<int,DomainA>") != parse_type("Result<int,DomainB>"),
          "nominal errors remain distinct");
    check(parse_type("Option<Result<int,DomainA>>") !=
              parse_type("Result<Option<int>,DomainA>"), "outer constructors remain distinct");

    shape(map(opt, i, parse_type("Result<int,DomainA>")), "Option<Result<int,DomainA>>");
    shape(map(res, i, parse_type("Option<int>")), "Result<Option<int>,DomainA>");
    shape(map(parse_type("Option<Result<int,DomainA>>"),
              parse_type("Result<int,DomainA>"), parse_type("Result<Tree<bool>,DomainA>")),
          "Option<Result<Tree<bool>,DomainA>>");
    shape(map(parse_type("Result<Option<int>,DomainA>"),
              opt, parse_type("Option<Option<int>>")),
          "Result<Option<Option<int>>,DomainA>");
    shape(map_err(res, parse_type("DomainA"), parse_type("DomainB")),
          "Result<int,DomainB>");
    shape(map_err(parse_type("Result<Option<int>,DomainA>"),
                  parse_type("DomainA"), parse_type("Tree<DomainB>")),
          "Result<Option<int>,Tree<DomainB>>");
    shape(and_then(opt, i, parse_type("Option<Tree<int>>")), "Option<Tree<int>>");
    shape(and_then(res, i, parse_type("Result<Option<int>,DomainA>")),
          "Result<Option<int>,DomainA>");
    shape(flatten(parse_type("Option<Option<int>>")), "Option<int>");
    shape(flatten(parse_type("Option<Option<Option<int>>>")), "Option<Option<int>>");
    shape(flatten(parse_type("Result<Result<int,DomainA>,DomainA>")),
          "Result<int,DomainA>");
    shape(flatten(parse_type("Result<Result<Result<int,DomainA>,DomainA>,DomainA>")),
          "Result<Result<int,DomainA>,DomainA>");
    shape(transpose(parse_type("Option<Result<int,DomainA>>")),
          "Result<Option<int>,DomainA>");
    shape(transpose(parse_type("Result<Option<int>,DomainA>")),
          "Option<Result<int,DomainA>>");
    shape(transpose(parse_type("Option<Result<Option<Tree<int>>,DomainA>>")),
          "Result<Option<Option<Tree<int>>>,DomainA>");
    shape(transpose(parse_type("Result<Option<Result<int,DomainB>>,DomainA>")),
          "Option<Result<Result<int,DomainB>,DomainA>>");

    // Shape-level functor identity and composition: F.map(id) and F.map(f).map(g)
    // agree with one map of g∘f. These are not runtime callback/side-effect laws.
    const Type nested = parse_type("Option<Result<int,DomainA>>");
    check(map(nested, nested.arguments[0], nested.arguments[0]) == nested,
          "map identity shape");
    const Type middle = parse_type("Result<Option<int>,DomainA>");
    const Type final_type = parse_type("Result<Option<Tree<bool>>,DomainA>");
    check(map(map(nested, nested.arguments[0], middle), middle, final_type) ==
              map(nested, nested.arguments[0], final_type), "map composition shape");
    check(transpose(transpose(parse_type("Option<Result<Option<int>,DomainA>>"))) ==
              parse_type("Option<Result<Option<int>,DomainA>>"),
          "transpose involution with inner Option");
    check(transpose(transpose(parse_type("Result<Option<Option<int>>,DomainA>"))) ==
              parse_type("Result<Option<Option<int>>,DomainA>"),
          "transpose involution with inner Option");

    // Affine admission policy checks are host-only, not ownership IR evidence.
    check_affine_rebuild(OwnerState::Owned, true);
    ++passed;
    check_affine_rebuild(OwnerState::SharedBorrow, false);
    ++passed;
    check_affine_rebuild(OwnerState::MutableBorrow, false);
    ++passed;
    reject("affine-borrow-escape", [] {
        check_affine_rebuild(OwnerState::SharedBorrow, true);
    });
    reject("affine-borrow-escape", [] {
        check_affine_rebuild(OwnerState::MutableBorrow, true);
    });
    reject("use-after-move", [] {
        check_affine_rebuild(OwnerState::Moved, false);
    });

    // Negative mutation controls must fail with explicit diagnostic categories.
    reject("wrong-callback-input-type", [&] {
        (void)map(opt, parse_type("bool"), i);
    });
    reject("wrong-callback-input-type", [&] {
        (void)map_err(res, parse_type("DomainB"), i);
    });
    reject("wrong-callback-input-type", [&] {
        (void)and_then(res, parse_type("bool"), res);
    });
    reject("wrong-return-type", [&] {
        (void)and_then(opt, i, res);
    });
    reject("incompatible-nominal-error-domain", [&] {
        (void)and_then(res, i, parse_type("Result<bool,DomainB>"));
    });
    reject("incompatible-nominal-error-domain", [] {
        (void)flatten(parse_type("Result<Result<int,DomainB>,DomainA>"));
    });
    reject("invalid-flatten-constructor", [&] { (void)flatten(res); });
    reject("invalid-flatten-constructor", [&] {
        (void)flatten(parse_type("Option<Result<int,DomainA>>"));
    });
    reject("invalid-transpose-constructor", [&] { (void)transpose(opt); });
    reject("invalid-transpose-constructor", [&] {
        (void)transpose(parse_type("Result<Result<int,DomainA>,DomainA>"));
    });
    reject("invalid-functor-receiver", [&] {
        (void)map(parse_type("Tree<int>"), i, parse_type("bool"));
    });
    reject("invalid-functor-receiver", [&] {
        (void)map_err(opt, i, parse_type("bool"));
    });
    reject("invalid-nested-type", [] { (void)parse_type("Option<Result<int,DomainA>"); });
    reject("invalid-nested-type", [] { (void)parse_type("Option<int>garbage"); });
    reject("invalid-type-arity", [] { (void)parse_type("Option<>"); });
    reject("invalid-type-arity", [] { (void)parse_type("Result<int>"); });
    reject("invalid-type-arity", [] { (void)parse_type("int<bool>"); });
    reject("unresolved-nested-generic", [] { (void)parse_type("Option<_>"); });
    reject("invalid-nested-type", [] {
        std::string layers;
        for (int n = 0; n < 34; ++n) layers += "Option<";
        layers += "int";
        for (int n = 0; n < 34; ++n) layers += ">";
        (void)parse_type(layers);
    });
    reject("invalid-nested-type", [] {
        (void)parse_type(std::string(4097, 'x'));
    });
    // Real LLVM compiler resolver boundary, still host-only (no guest IR).
    oreslang::Program admitted;
    admitted.sum_types.push_back({oreslang::NativeSumType::Kind::Option,
                                  "Option<int>", oreslang::ScalarType::I64, oreslang::ScalarType::I64});
    admitted.sum_types.push_back({oreslang::NativeSumType::Kind::Result,
                                  "Result<bool,String>", oreslang::ScalarType::Bool, oreslang::ScalarType::String});
    oreslang::resolve_native_sums(admitted);
    check(admitted.sum_types.size() == 2, "canonical scalar sums admitted");

    auto reject_descriptor = [](oreslang::NativeSumType descriptor) {
        oreslang::Program invalid;
        invalid.sum_types.push_back(std::move(descriptor));
        try {
            oreslang::resolve_native_sums(invalid);
        } catch (const std::runtime_error& error) {
            check(std::string(error.what()).find("native sum descriptor type-shape mismatch") != std::string::npos,
                  "bad sum descriptor rejected before IR with precise message");
            return;
        }
        throw std::runtime_error("native sum resolver accepted malformed descriptor");
    };
    reject_descriptor({oreslang::NativeSumType::Kind::Option, "Option<bool>",
                       oreslang::ScalarType::I64, oreslang::ScalarType::I64});
    reject_descriptor({oreslang::NativeSumType::Kind::Result, "Result<int,DomainB>",
                       oreslang::ScalarType::I64, oreslang::ScalarType::String});
    reject_descriptor({oreslang::NativeSumType::Kind::Option, "Option<Result<int,String>>",
                       oreslang::ScalarType::I64, oreslang::ScalarType::I64});
    std::cout << "PASS " << passed << " compiler-type-shape host-only checks; "
              << "LLVM guest/native machine-code verification NOT_EXECUTED\n";
    return 0;
}
