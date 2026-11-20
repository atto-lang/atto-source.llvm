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
                target->emitted_full_symbol != "identity$atto_g1$int" ||
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
                forged.emitted_full_symbol = "shadow.identity$atto_g1$int";
                // The frozen factory binds this to the emitted module name;
                // this mutation is caught at the module/overload planner.
                must_reject([&] {
                    (void)verify_native_i64_scalar_call_witness(
                        forged, selector, original->emitted_full_symbol,
                        original->emitted_full_symbol, 1, {"value"}, true);
                }, "native AOT static call witness identity mismatch");
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
        // The actual LLVM symbol plan must include module ownership,
        // overload selector and frozen concrete tuple, without trusting a
        // caller-supplied host linkage name.
        {
            auto left = fixture();
            left.module = "payments";
            const auto frozen = freeze_native_i64_dispatch_image(left);
            const auto symbol = plan_native_aot_function_symbol(
                left, left.functions[0], frozen, 1, false, false);
            if (symbol != "payments.identity$atto_g1$int" ||
                frozen.find_direct(native_function_slot("identity", 1))->emitted_full_symbol != symbol)
                throw std::runtime_error("qualified native generic ABI symbol mismatch");
            auto right = fixture();
            right.module = "inventory";
            const auto other = freeze_native_i64_dispatch_image(right);
            if (symbol == plan_native_aot_function_symbol(
                    right, right.functions[0], other, 1, false, false))
                throw std::runtime_error("two linked module identities share an ABI symbol");
        }
        {
            auto program = fixture();
            program.module = "payments";
            Function overloaded = generic("identity");
            Parameter second;
            second.name = "scale";
            overloaded.parameters.push_back(std::move(second));
            program.functions.push_back(std::move(overloaded));
            const auto image = freeze_native_i64_dispatch_image(program);
            const auto one = plan_native_aot_function_symbol(
                program, program.functions[0], image, 2, false, false);
            const auto two = plan_native_aot_function_symbol(
                program, program.functions[1], image, 2, false, false);
            if (one != "payments.identity$arity1$atto_g1$int" ||
                two != "payments.identity$arity2$atto_g1$int" || one == two)
                throw std::runtime_error("overloaded native direct-call symbol collision");
        }
        // Do not reserve the AttoLang entrypoint itself on the browser Wasm
        // target. Native and Cortex-M have their own approved entry rewrites.
        {
            Program entry;
            Function main_fn;
            main_fn.name = "main";
            entry.functions.push_back(std::move(main_fn));
            const auto image = freeze_native_i64_dispatch_image(entry);
            const auto& fn = entry.functions[0];
            if (plan_native_aot_function_symbol(entry, fn, image, 1, false, false) != "main" ||
                plan_native_aot_function_symbol(entry, fn, image, 1, true, false) !=
                    "__oreslang_guest_main" ||
                plan_native_aot_function_symbol(entry, fn, image, 1, false, true) !=
                    "oreslang_cortexm_bootstrap")
                throw std::runtime_error("native/wasm/cortex entry symbol rewriting drift");
            entry.module = "payments";
            if (plan_native_aot_function_symbol(entry, fn, image, 1, true, false) != "payments.main")
                throw std::runtime_error("module-qualified main ABI lost module identity");
        }
        // Runtime/host linker names must be rejected even if an earlier
        // frontend admitted a forged compiler-owned nongeneric declaration.
        for (const std::string name : {
                 "abort", "write", "ores_record_get", "ores_string_print",
                 "oreslang_source_actor_send", "__oreslang_guest_main", "ores_guest_entry"}) {
            Program program;
            Function fn;
            fn.name = name;
            program.functions.push_back(std::move(fn));
            const auto image = freeze_native_i64_dispatch_image(program);
            must_reject([&] {
                (void)plan_native_aot_function_symbol(
                    program, program.functions[0], image, 1, false, false);
            }, "reserved native AOT runtime symbol collision");
        }
        // A module-qualified user method with the same unqualified spelling
        // has an independent linker identity and must not be rejected.
        {
            Program program;
            program.module = "payments";
            Function fn;
            fn.name = "write";
            program.functions.push_back(std::move(fn));
            const auto image = freeze_native_i64_dispatch_image(program);
            if (plan_native_aot_function_symbol(
                    program, program.functions[0], image, 1, false, false) !=
                    "payments.write")
                throw std::runtime_error("qualified user symbol shadowed unrelated host ABI");
        }
        {
            Program program;
            program.module = "llvm";
            Function fn;
            fn.name = "trap";
            program.functions.push_back(std::move(fn));
            const auto image = freeze_native_i64_dispatch_image(program);
            must_reject([&] {
                (void)plan_native_aot_function_symbol(
                    program, program.functions[0], image, 1, false, false);
            }, "reserved LLVM intrinsic symbol collision");
        }
        {
            auto program = fixture();
            program.module = std::string("tenant\0spoof", 12);
            const auto image = freeze_native_i64_dispatch_image(program);
            must_reject([&] {
                (void)plan_native_aot_function_symbol(
                    program, program.functions[0], image, 1, false, false);
            }, "invalid native AOT emitted symbol identity");
        }
        {
            auto program = fixture();
            const auto image = freeze_native_i64_dispatch_image(program);
            must_reject([&] {
                (void)plan_native_aot_function_symbol(
                    program, program.functions[0], image, 0, false, false);
            }, "native AOT missing compiler function family");
        }
        // Real lowering retains LLVM Function* identity and checks this
        // frozen scalar witness on every admitted generic call. These
        // source-level mutations test the metadata gate; actual pointer
        // binding still requires an executed LLVM compiler/guest test.
        {
            const auto image = freeze_native_i64_dispatch_image(source);
            const auto* target = image.find_direct(selector);
            if (!target) throw std::runtime_error("missing scalar call witness");
            const auto symbol = plan_native_aot_function_symbol(
                source, source.functions[0], image, 1, false, false);
            const std::vector<std::string> labels = {"value"};
            verify_native_i64_scalar_call_witness(
                *target, selector, symbol, symbol, 1, labels, true);
            const auto reject = [&](const std::string& slot,
                                    const std::string& recorded,
                                    const std::string& actual,
                                    std::size_t arity,
                                    const std::vector<std::string>& formals,
                                    bool internal) {
                must_reject([&] {
                    verify_native_i64_scalar_call_witness(
                        *target, slot, recorded, actual, arity, formals, internal);
                }, "native AOT static call witness");
            };
            reject(native_function_slot("other", 1), symbol, symbol, 1, labels, true);
            reject(selector, symbol, symbol, 2, labels, true);
            reject(selector, symbol, "shadow." + symbol, 1, labels, true);
            reject(selector, "shadow." + symbol, "shadow." + symbol, 1, labels, true);
            reject(selector, symbol, symbol, 1, {"wrong"}, true);
            reject(selector, symbol, symbol, 1, {}, true);
            reject(selector, symbol, symbol, 1, labels, false);
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
        std::cout << "NATIVE_I64_SPECIALIZATION_PLAN|symbols=1|negative_cases=45\n";
    } catch (const std::exception& error) {
        std::cerr << "NATIVE_I64_SPECIALIZATION_PLAN_FAIL: " << error.what() << '\n';
        return 1;
    }
}
