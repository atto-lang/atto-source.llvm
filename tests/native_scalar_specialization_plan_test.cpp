#include "oreslang/frontend/NativeScalarSpecialization.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
using namespace oreslang;

template<class Fn>
void must_reject(Fn&& run, const std::string& fragment) {
    try {
        std::forward<Fn>(run)();
    } catch (const std::runtime_error& error) {
        if (std::string(error.what()).find(fragment) != std::string::npos) return;
        throw std::runtime_error("unexpected specialization denial: " +
                                 std::string(error.what()));
    }
    throw std::runtime_error("unsafe native generic specialization accepted: " + fragment);
}

Function generic(const std::string& name) {
    Function fn;
    fn.name = name;
    fn.generic_parameters.push_back({"T", ""});
    Parameter arg;
    arg.name = "value";
    arg.generic_type = "T";
    fn.parameters.push_back(std::move(arg));
    return fn;
}

Program fixture() {
    Program result;
    result.functions.push_back(generic("identity"));
    return result;
}
} // namespace

int main() {
    try {
        auto source = fixture();
        auto proof = freeze_native_i64_specializations(source);
        const std::string selector = native_function_slot("identity", 1);
        if (proof.size() != 1 || proof.at(selector) != "$atto_g1$int")
            throw std::runtime_error("canonical i64 generic ABI mismatch");
        if (freeze_native_i64_specializations(source).at(selector) != proof.at(selector))
            throw std::runtime_error("specialization identity is not deterministic");
        // Production LLVM codegen consumes the full frozen image, rather
        // than deriving authority from a source-authored suffix or runtime map.
        {
            const auto image = freeze_native_i64_dispatch_image(source);
            const auto* target = image.find_direct(selector);
            if (image.direct_targets.size() != 1 || target == nullptr ||
                target->selector != selector ||
                target->emitted_suffix != "$atto_g1$int" ||
                target->lowered_arity != 1 ||
                target->concrete_types != std::vector<std::string>{"int"} ||
                image.closure.concrete_symbols != 1 ||
                image.closure.estimated_code_bytes == 0 ||
                image.closure.dependencies.size() != 1 ||
                !image.closure.dependencies.at(selector).empty())
                throw std::runtime_error("frozen compiler-owned dispatch image does not match typed i64 ABI");
            if (image.find_direct(native_function_slot("missing", 0)) != nullptr)
                throw std::runtime_error("unproved direct target exists in frozen dispatch image");
        }
        // Tamper each frozen ABI dimension independently. Codegen must
        // never trust a suffix-only or arity-only certificate when
        // borrow modes, labels, return type or effects have changed.
        {
            const auto image = freeze_native_i64_dispatch_image(source);
            const auto* original = image.find_direct(selector);
            if (!original)
                throw std::runtime_error("missing canonical frozen ABI target");
            const auto reject_mutation = [&](NativeI64DirectTarget forged) {
                must_reject([&] {
                    verify_native_i64_direct_target(source.functions[0], forged);
                }, "native AOT frozen direct dispatch ABI mismatch");
            };
            {
                auto forged = *original;
                forged.parameter_abi[0].mode = Parameter::Mode::BorrowMut;
                reject_mutation(std::move(forged));
            }
            {
                auto forged = *original;
                forged.parameter_abi[0].name = "renamed";
                reject_mutation(std::move(forged));
            }
            {
                auto forged = *original;
                forged.parameter_abi[0].scalar_type = ScalarType::Bool;
                reject_mutation(std::move(forged));
            }
            {
                auto forged = *original;
                forged.parameter_abi[0].generic_type = "Undeclared";
                reject_mutation(std::move(forged));
            }
            {
                auto forged = *original;
                forged.return_type = ScalarType::String;
                reject_mutation(std::move(forged));
            }
            {
                auto forged = *original;
                forged.returns_void = true;
                reject_mutation(std::move(forged));
            }
            {
                auto forged = *original;
                forged.declared_effects.bits =
                    static_cast<std::uint32_t>(FunctionEffect::Io);
                reject_mutation(std::move(forged));
            }
            {
                auto forged = *original;
                forged.emitted_suffix = "$atto_g1$bool";
                reject_mutation(std::move(forged));
            }
            {
                auto forged = *original;
                forged.exported = !forged.exported;
                reject_mutation(std::move(forged));
            }
        }
        // Unknown effect bits and unresolved return aliases are invalid
        // even if injected after the source type-checking pass.
        {
            auto bad = fixture();
            bad.functions[0].has_declared_effects = true;
            bad.functions[0].declared_effects = FunctionEffects::unresolved_call();
            must_reject([&] { (void)freeze_native_i64_dispatch_image(bad); },
                        "native AOT frozen direct dispatch ABI mismatch");
        }
        {
            auto bad = fixture();
            bad.functions[0].pending_return_type_alias = "Unresolved";
            must_reject([&] { (void)freeze_native_i64_dispatch_image(bad); },
                        "native AOT frozen direct dispatch ABI mismatch");
        }
        // A valid, previously admitted effect/borrow combination remains
        // representable in the frozen metadata (not a guest semantic proof).
        {
            auto typed = fixture();
            typed.functions[0].parameters[0].mode = Parameter::Mode::Borrow;
            typed.functions[0].has_declared_effects = true;
            typed.functions[0].declared_effects = {
                static_cast<std::uint32_t>(FunctionEffect::StateRead)
            };
            const auto image = freeze_native_i64_dispatch_image(typed);
            const auto* target = image.find_direct(selector);
            if (!target || target->parameter_abi[0].mode != Parameter::Mode::Borrow ||
                !target->has_declared_effects ||
                target->declared_effects.bits != typed.functions[0].declared_effects.bits)
                throw std::runtime_error("valid borrowed/effect ABI was not frozen");
            verify_native_i64_direct_target(typed.functions[0], *target);
        }
        // The concrete tuple remains declaration-ordered even when defaults
        // refer to earlier parameters. No runtime generic expansion occurs.
        {
            Program dependent;
            Function fn = generic("depend");
            fn.generic_parameters.push_back({"U", "T"});
            dependent.functions.push_back(std::move(fn));
            const auto image = freeze_native_i64_dispatch_image(dependent);
            const auto* target = image.find_direct(native_function_slot("depend", 1));
            if (target == nullptr || target->emitted_suffix != "$atto_g2$int$int" ||
                target->concrete_types != std::vector<std::string>({"int", "int"}))
                throw std::runtime_error("dependent concrete i64 tuple was not frozen in order");
        }
        // The same selected source function/arity is used by all call sites;
        // the producer does not manufacture one method body per call site.
        {
            auto bad = fixture();
            bad.functions[0].generic_parameters[0].name = "T$forged";
            must_reject([&] { (void)freeze_native_i64_specializations(bad); },
                        "invalid native AOT generic parameter identity");
        }
        {
            auto bad = fixture();
            bad.functions[0].generic_parameters.push_back({"T", ""});
            must_reject([&] { (void)freeze_native_i64_specializations(bad); },
                        "invalid native AOT generic parameter identity");
        }
        {
            auto bad = fixture();
            bad.functions[0].generic_parameters[0].default_type = "T";
            must_reject([&] { (void)freeze_native_i64_specializations(bad); },
                        "invalid native AOT generic default dependency");
        }
        {
            auto bad = fixture();
            bad.functions[0].parameters[0].generic_type = "Unknown";
            must_reject([&] { (void)freeze_native_i64_specializations(bad); },
                        "unresolved native AOT generic parameter ABI");
        }
        {
            auto bad = fixture();
            bad.functions[0].parameters[0].class_name = "Other";
            must_reject([&] { (void)freeze_native_i64_specializations(bad); },
                        "unresolved native AOT generic parameter ABI");
        }
        {
            auto bad = fixture();
            bad.functions[0].method_owner = "Box";
            must_reject([&] { (void)freeze_native_i64_specializations(bad); },
                        "native AOT generic method or arity not supported");
        }
        {
            auto bad = fixture();
            bad.functions.push_back(generic("identity"));
            must_reject([&] { (void)freeze_native_i64_specializations(bad); },
                        "duplicate compiler-owned native function selector");
        }
        // Internal codegen uses name+arity selectors for all functions,
        // including non-generics. Mixed declarations must fail before ABI
        // publication; a generic suffix cannot rescue a clashing selector.
        {
            auto bad = fixture();
            Function nongeneric;
            nongeneric.name = "identity";
            Parameter arg;
            arg.name = "value";
            nongeneric.parameters.push_back(std::move(arg));
            bad.functions.push_back(std::move(nongeneric));
            must_reject([&] { (void)freeze_native_i64_specializations(bad); },
                        "duplicate compiler-owned native function selector");
        }
        // Post-checker IR may carry forged scalar tags or stale aliases on
        // an otherwise valid T parameter. No erased int ABI is allowed.
        {
            auto bad = fixture();
            bad.functions[0].parameters[0].scalar_type = ScalarType::Bool;
            must_reject([&] { (void)freeze_native_i64_specializations(bad); },
                        "unresolved native AOT generic parameter ABI");
        }
        {
            auto bad = fixture();
            bad.functions[0].parameters[0].scalar_type = ScalarType::String;
            must_reject([&] { (void)freeze_native_i64_specializations(bad); },
                        "unresolved native AOT generic parameter ABI");
        }
        {
            auto bad = fixture();
            bad.functions[0].parameters[0].pending_type_alias = "Alias";
            must_reject([&] { (void)freeze_native_i64_specializations(bad); },
                        "unresolved native AOT generic parameter ABI");
        }
        {
            auto bad = fixture();
            bad.functions[0].parameters[0].sum_type = "Option";
            must_reject([&] { (void)freeze_native_i64_specializations(bad); },
                        "unresolved native AOT generic parameter ABI");
        }
        {
            Program bad;
            for (int i = 0; i < 257; ++i)
                bad.functions.push_back(generic("name" + std::to_string(i)));
            must_reject([&] { (void)freeze_native_i64_specializations(bad); },
                        "native AOT generic specialization count exceeded");
        }
        // A non-generic function is a legal empty specialization closure.
        {
            Program plain;
            Function fn;
            fn.name = "plain";
            Parameter parameter;
            parameter.name = "n";
            fn.parameters.push_back(std::move(parameter));
            plain.functions.push_back(std::move(fn));
            if (!freeze_native_i64_specializations(plain).empty())
                throw std::runtime_error("plain function minted generic symbols");
            const auto image = freeze_native_i64_dispatch_image(plain);
            if (!image.direct_targets.empty() || image.closure.concrete_symbols != 0 ||
                !image.closure.dependencies.empty())
                throw std::runtime_error("plain function acquired forged direct dispatch targets");
        }
        // Post-validation compiler IR mutation must never silently erase a
        // forged generic parameter into an ordinary i64 ABI slot.
        {
            Program forged;
            Function fn;
            fn.name = "forged";
            Parameter parameter;
            parameter.name = "value";
            parameter.generic_type = "T"; // no declaring generic parameter
            fn.parameters.push_back(std::move(parameter));
            forged.functions.push_back(std::move(fn));
            must_reject([&] { (void)freeze_native_i64_specializations(forged); },
                        "undeclared native AOT generic parameter ABI");
        }
        // Compiler-owned generic calls form a bounded, concrete dependency
        // graph. Two calls to the same specialization create only one edge.
        {
            auto program = fixture();
            program.functions.push_back(generic("helper"));
            auto add = std::make_unique<Expression>();
            add->kind = Expression::Kind::Add;
            for (auto* slot : {&add->left, &add->right}) {
                auto call = std::make_unique<Expression>();
                call->kind = Expression::Kind::Call;
                call->name = "helper";
                auto arg = std::make_unique<Expression>();
                arg->kind = Expression::Kind::Integer;
                arg->integer = 1;
                call->arguments.push_back(std::move(arg));
                *slot = std::move(call);
            }
            program.functions[0].result = std::move(add);
            const auto symbols = freeze_native_i64_specializations(program);
            const auto closure = check_native_i64_specialization_closure(program, symbols);
            if (symbols.size() != 2 || closure.concrete_symbols != 2 ||
                closure.dependency_edges != 1 || closure.estimated_code_bytes == 0 ||
                closure.dependencies.at(native_function_slot("identity", 1)) !=
                    std::vector<std::string>{native_function_slot("helper", 1)} ||
                !closure.dependencies.at(native_function_slot("helper", 1)).empty())
                throw std::runtime_error("reused generic dependency was not deduplicated");
        }
        // Cycles in a fixed emitted specialization graph are legal. They do
        // not cause unbounded runtime creation of new generic symbols.
        {
            auto program = fixture();
            program.functions.push_back(generic("helper"));
            for (std::size_t i = 0; i < program.functions.size(); ++i) {
                auto call = std::make_unique<Expression>();
                call->kind = Expression::Kind::Call;
                call->name = i == 0 ? "helper" : "identity";
                auto arg = std::make_unique<Expression>();
                arg->kind = Expression::Kind::Integer;
                arg->integer = 42;
                call->arguments.push_back(std::move(arg));
                program.functions[i].result = std::move(call);
            }
            const auto symbols = freeze_native_i64_specializations(program);
            const auto closure = check_native_i64_specialization_closure(program, symbols);
            if (closure.dependency_edges != 2 || closure.concrete_symbols != 2 ||
                closure.dependencies.at(native_function_slot("identity", 1)) !=
                    std::vector<std::string>{native_function_slot("helper", 1)} ||
                closure.dependencies.at(native_function_slot("helper", 1)) !=
                    std::vector<std::string>{native_function_slot("identity", 1)})
                throw std::runtime_error("bounded recursive specialization closure mismatch");
        }
        // The checker must never treat a source/compiler-supplied symbol map
        // as proof that an implementation body exists.
        {
            Program absent;
            std::unordered_map<std::string, std::string> forged = {
                {native_function_slot("missing", 0), "$atto_g1$int"}
            };
            must_reject([&] {
                (void)check_native_i64_specialization_closure(absent, forged);
            }, "missing emitted native AOT specialization node");
        }
        // A large nongeneric body must not deplete the specialization-only
        // closure scan allowance, even when the module also declares a
        // generic. It is checked elsewhere by the ordinary compiler passes.
        {
            auto program = fixture();
            Function plain;
            plain.name = "unrelated";
            auto enormous = std::make_unique<Expression>();
            enormous->kind = Expression::Kind::Integer;
            enormous->arguments.reserve(131073);
            for (int i = 0; i < 131073; ++i) {
                auto child = std::make_unique<Expression>();
                child->kind = Expression::Kind::Integer;
                enormous->arguments.push_back(std::move(child));
            }
            plain.result = std::move(enormous);
            program.functions.push_back(std::move(plain));
            const auto image = freeze_native_i64_dispatch_image(program);
            if (image.closure.concrete_symbols != 1 ||
                image.closure.scanned_nodes != 0 ||
                image.closure.estimated_code_bytes != 256)
                throw std::runtime_error("nongeneric AST consumed generic scan budget");
        }
        // Deliberately malformed compiler-owned AST, flat rather than deep.
        // Exactly 131072 scanned nodes fit the scan budget, while the
        // independent 8 MiB specialization byte estimate must reject it.
        {
            auto oversized = fixture();
            auto container = std::make_unique<Expression>();
            container->kind = Expression::Kind::Integer;
            container->arguments.reserve(131071);
            for (int i = 0; i < 131071; ++i) {
                auto child = std::make_unique<Expression>();
                child->kind = Expression::Kind::Integer;
                container->arguments.push_back(std::move(child));
            }
            oversized.functions[0].result = std::move(container);
            must_reject([&] { (void)freeze_native_i64_specializations(oversized); },
                        "native AOT specialization byte estimate exceeded");
        }
        std::cout << "NATIVE_I64_SPECIALIZATION_PLAN|symbols=1|negative_cases=27\n";
    } catch (const std::exception& error) {
        std::cerr << "NATIVE_I64_SPECIALIZATION_PLAN_FAIL: " << error.what() << '\n';
        return 1;
    }
}
