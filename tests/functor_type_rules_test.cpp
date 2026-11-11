#include "oreslang/types/FunctorTypeRules.hpp"
#include "oreslang/types/NativeSumTypes.hpp"

#include <functional>
#include <iostream>
#include <memory>
#include <utility>
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
    reject("invalid-owner-state", [] {
        check_affine_rebuild(static_cast<OwnerState>(999), false);
    });
    reject("invalid-owner-state", [] {
        check_affine_rebuild(static_cast<OwnerState>(999), true);
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
    // Programmatically synthesized IR types must obey the same structural
    // admission rules as text parsed from .atto. Otherwise a later pass could
    // smuggle arity errors, inferred holes, and invalid qualified names.
    reject("invalid-type-arity", [&] {
        (void)map(opt, i, Type{"Option", {}});
    });
    reject("invalid-type-arity", [&] {
        (void)map(opt, i, Type{"Result", {i}});
    });
    reject("invalid-nested-type", [&] {
        (void)map(opt, i, Type{"Record..Nested", {}});
    });
    reject("invalid-nested-type", [&] {
        (void)map_err(res, parse_type("DomainA"), Type{".Type", {}});
    });
    reject("unresolved-nested-generic", [&] {
        (void)map(opt, i, Type{"Result", {i, Type{"_", {}}}});
    });
    reject("invalid-functor-payload", [&] {
        (void)map(opt, i, parse_type("void"));
    });
    shape(parse_type("void"), "void");
    reject("invalid-functor-payload", [] {
        (void)parse_type("Option<Result<void,DomainA>>");
    });
    reject("invalid-functor-payload", [] {
        (void)parse_type("Result<int,void>");
    });
    reject("invalid-functor-payload", [&] {
        (void)map(opt, i, Type{"Result", {i, Type{"void", {}}}});
    });
    reject("invalid-type-arity", [&] {
        (void)and_then(opt, i, Type{"Option", {i, i}});
    });
    reject("invalid-nested-type", [] {
        (void)parse_type("Pkg..Type");
    });
    reject("invalid-nested-type", [] {
        (void)parse_type("Pkg.Type.");
    });
    reject("invalid-nested-type", [] {
        (void)parse_type("Pkg.2Type");
    });
    reject("invalid-nested-type", [&] {
        (void)map(Type{"Option", {Type{"Record..Payload", {}}}}, i, i);
    });
    reject("invalid-nested-type", [&] {
        Type deep = i;
        for (unsigned n = 0; n != 34; ++n) deep = Type{"Wrapper", {deep}};
        (void)map(opt, i, deep);
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
    // Native ABI registry hardening: opaque enum values, hidden Option errors,
    // duplicate names and fabricated nested descriptors must fail before IR.
    auto reject_table = [](oreslang::Program table, const std::string& reason) {
        try {
            oreslang::resolve_native_sums(table);
        } catch (const std::runtime_error& error) {
            check(std::string(error.what()).find(reason) != std::string::npos,
                  "invalid native sum registry gives expected diagnostic");
            return;
        }
        throw std::runtime_error("accepted an invalid scalar sum descriptor table");
    };
    {
        oreslang::Program duplicate;
        duplicate.sum_types.push_back({oreslang::NativeSumType::Kind::Option,
                                       "Option<int>", oreslang::ScalarType::I64, oreslang::ScalarType::I64});
        duplicate.sum_types.push_back(duplicate.sum_types.front());
        reject_table(std::move(duplicate), "duplicate native sum type descriptor");
    }
    {
        oreslang::Program forged;
        forged.sum_types.push_back({static_cast<oreslang::NativeSumType::Kind>(99),
                                    "Result<int,int>", oreslang::ScalarType::I64, oreslang::ScalarType::I64});
        reject_table(std::move(forged), "invalid native sum scalar ABI descriptor");
    }
    {
        oreslang::Program forged;
        forged.sum_types.push_back({oreslang::NativeSumType::Kind::Option,
                                    "Option<int>", static_cast<oreslang::ScalarType>(99), oreslang::ScalarType::I64});
        reject_table(std::move(forged), "invalid native sum scalar ABI descriptor");
    }
    {
        oreslang::Program forged;
        forged.sum_types.push_back({oreslang::NativeSumType::Kind::Result,
                                    "Result<int,int>", oreslang::ScalarType::I64, static_cast<oreslang::ScalarType>(99)});
        reject_table(std::move(forged), "invalid native sum scalar ABI descriptor");
    }
    {
        oreslang::Program forged;
        forged.sum_types.push_back({oreslang::NativeSumType::Kind::Option,
                                    "Option<int>", oreslang::ScalarType::I64, oreslang::ScalarType::String});
        reject_table(std::move(forged), "invalid native sum scalar ABI descriptor");
    }
    // Construct a syntactically valid AST and then mutate its unused fields:
    // the production sum resolver must fail before the compiler lowers them.
    auto sum_ast = [] {
        oreslang::Program program;
        program.sum_types.push_back({oreslang::NativeSumType::Kind::Option,
                                     "Option<int>", oreslang::ScalarType::I64, oreslang::ScalarType::I64});
        oreslang::Function fn;
        fn.name = "sum_ast";
        oreslang::LocalBinding local;
        local.name = "value";
        local.sum_type = "Option<int>";
        local.initializer = std::make_unique<oreslang::Expression>();
        local.initializer->kind = oreslang::Expression::Kind::SumConstruct;
        local.initializer->name = "Some";
        auto value = std::make_unique<oreslang::Expression>();
        value->kind = oreslang::Expression::Kind::Integer;
        local.initializer->arguments.push_back(std::move(value));
        fn.locals.push_back(std::move(local));
        program.functions.push_back(std::move(fn));
        return program;
    };
    {
        auto valid = sum_ast();
        oreslang::resolve_native_sums(valid);
        check(valid.functions.front().locals.front().initializer->sum_type == "Option<int>",
              "well-formed sum constructor AST still admitted");
    }
    auto reject_bad_sum_ast = [&](const std::function<void(oreslang::Expression&)>& mutate) {
        auto program = sum_ast();
        mutate(*program.functions.front().locals.front().initializer);
        reject_table(std::move(program), "malformed native sum constructor AST");
    };
    reject_bad_sum_ast([](oreslang::Expression& sum) {
        sum.left = std::make_unique<oreslang::Expression>();
        sum.left->kind = oreslang::Expression::Kind::String;
    });
    reject_bad_sum_ast([](oreslang::Expression& sum) {
        sum.when_false = std::make_unique<oreslang::Expression>();
        sum.when_false->kind = oreslang::Expression::Kind::Integer;
    });
    reject_bad_sum_ast([](oreslang::Expression& sum) {
        sum.type_arguments_present = true;
    });
    reject_bad_sum_ast([](oreslang::Expression& sum) {
        sum.type_arguments.push_back({"T", "int", false});
    });
    reject_bad_sum_ast([](oreslang::Expression& sum) {
        sum.argument_names.push_back("payload");
    });
    reject_bad_sum_ast([](oreslang::Expression& sum) {
        sum.argument_names.push_back("");
        sum.argument_names.push_back("");
    });
    reject_bad_sum_ast([](oreslang::Expression& sum) {
        sum.arguments.front().reset();
    });
    {
        auto program = sum_ast();
        oreslang::LocalBinding invalid;
        invalid.name = "missing_receiver";
        invalid.initializer = std::make_unique<oreslang::Expression>();
        invalid.initializer->kind = oreslang::Expression::Kind::ArrayCall;
        invalid.initializer->name = "unwrap";
        program.functions.front().locals.push_back(std::move(invalid));
        reject_table(std::move(program), "malformed native sum/member access AST");
    }
    {
        auto program = sum_ast();
        oreslang::LocalBinding invalid;
        invalid.name = "missing_argument";
        invalid.initializer = std::make_unique<oreslang::Expression>();
        invalid.initializer->kind = oreslang::Expression::Kind::ArrayCall;
        invalid.initializer->name = "unwrap_or";
        invalid.initializer->left = std::make_unique<oreslang::Expression>();
        invalid.initializer->left->kind = oreslang::Expression::Kind::Variable;
        invalid.initializer->left->name = "value";
        invalid.initializer->arguments.emplace_back(); // intentionally null
        program.functions.front().locals.push_back(std::move(invalid));
        reject_table(std::move(program), "malformed native sum/member access AST");
    }


    auto reject_hidden_scalar_payload = [&](const std::function<void(oreslang::Expression&)>& mutate) {
        auto program = sum_ast();
        auto& payload = *program.functions.front().locals.front().initializer->arguments.front();
        mutate(payload);
        reject_table(std::move(program), "malformed native scalar leaf AST");
    };
    reject_hidden_scalar_payload([](oreslang::Expression& payload) {
        payload.left = std::make_unique<oreslang::Expression>();
        payload.left->kind = oreslang::Expression::Kind::Call;
    });
    reject_hidden_scalar_payload([](oreslang::Expression& payload) {
        payload.when_true = std::make_unique<oreslang::Expression>();
        payload.when_true->kind = oreslang::Expression::Kind::String;
    });
    reject_hidden_scalar_payload([](oreslang::Expression& payload) {
        payload.type_arguments_present = true;
    });
    reject_hidden_scalar_payload([](oreslang::Expression& payload) {
        payload.arguments.push_back(std::make_unique<oreslang::Expression>());
    });
    // Exercise the exact ABI merge helper used by the real module loader.
    // Repeated identical descriptors from separate imports are legal; a
    // conflicting descriptor with the same name must never win by order.
    std::vector<oreslang::NativeSumType> linked;
    const oreslang::NativeSumType option_int{
        oreslang::NativeSumType::Kind::Option,
        "Option<int>", oreslang::ScalarType::I64, oreslang::ScalarType::I64
    };
    oreslang::merge_native_sum_descriptor(linked, option_int);
    oreslang::merge_native_sum_descriptor(linked, option_int);
    check(linked.size() == 1, "identical imported Option<int> descriptors coalesced");
    oreslang::merge_native_sum_descriptor(linked,
        {oreslang::NativeSumType::Kind::Result, "Result<bool,String>",
         oreslang::ScalarType::Bool, oreslang::ScalarType::String});
    check(linked.size() == 2, "distinct imported sum descriptors retained");
    try {
        oreslang::merge_native_sum_descriptor(linked,
            {oreslang::NativeSumType::Kind::Option, "Option<int>",
             oreslang::ScalarType::String, oreslang::ScalarType::I64});
        throw std::runtime_error("conflicting imported ABI descriptor was accepted");
    } catch (const std::runtime_error& error) {
        check(std::string(error.what()).find("conflicting linked native sum type descriptor") !=
                  std::string::npos, "conflicting imported sum ABI rejected");
    }
    std::cout << "PASS " << passed << " compiler-type-shape host-only checks; "
              << "LLVM guest/native machine-code verification NOT_EXECUTED\n";
    return 0;
}
