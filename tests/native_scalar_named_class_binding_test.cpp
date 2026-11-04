// Host-only execution of the production native scalar named-class slot binder.
// Does not prove compiler, LLVM IR or guest execution parity.
#include "oreslang/frontend/Parser.hpp"

#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using oreslang::ClassDecl;
using oreslang::GenericParameter;
using oreslang::GenericTypeArgument;
using oreslang::LocalBinding;
using oreslang::native_bind_scalar_class_generics;
using oreslang::native_scalar_class_generic_slots;
using oreslang::native_verify_local_class_specialization;

int checks = 0;

ClassDecl cls(std::initializer_list<GenericParameter> specs) {
    ClassDecl c{};
    c.name = "Pair";
    c.generic_parameters.assign(specs.begin(), specs.end());
    return c;
}
GenericTypeArgument positional(const std::string& type = "int") {
    return {"", type, false};
}
GenericTypeArgument named(const std::string& label, const std::string& type = "int") {
    return {label, type, false};
}
GenericTypeArgument hole(const std::string& label) {
    return {label, "", true};
}

void accepted(const ClassDecl& c, std::initializer_list<GenericTypeArgument> args,
              const std::vector<std::string>& expected) {
    const std::vector<GenericTypeArgument> provided(args.begin(), args.end());
    auto actual = native_scalar_class_generic_slots(c, provided);
    if (actual != expected)
        throw std::runtime_error("incorrect nominal generic declaration-slot binding");
    ++checks;
}
// Unlike type-value equality, these assertions observe the actual mapping
// from lexical named arguments to distinct declaration generic identities.
void mapped(const ClassDecl& c, std::initializer_list<GenericTypeArgument> args,
            std::initializer_list<std::size_t> expected) {
    const std::vector<GenericTypeArgument> provided(args.begin(), args.end());
    const auto binding = native_bind_scalar_class_generics(c, provided);
    const std::vector<std::size_t> wanted(expected.begin(), expected.end());
    if (binding.source_to_formal != wanted)
        throw std::runtime_error("named class generic source-to-formal order mismatch");
    if (binding.formal_types.size() != c.generic_parameters.size())
        throw std::runtime_error("named class generic canonical arity mismatch");
    ++checks;
}

void rejected(const ClassDecl& c, std::initializer_list<GenericTypeArgument> args,
              const std::string& error) {
    try {
        const std::vector<GenericTypeArgument> provided(args.begin(), args.end());
        (void)native_scalar_class_generic_slots(c, provided);
    } catch (const std::runtime_error& ex) {
        if (std::string(ex.what()).find(error) == std::string::npos)
            throw std::runtime_error("wrong nominal generic rejection: " +
                                     std::string(ex.what()));
        ++checks;
        return;
    }
    throw std::runtime_error("invalid nominal class specialization admitted");
}
} // namespace

int main() {
    try {
        const auto pair = cls({{"A", ""}, {"B", ""}});
        const auto defaults = cls({{"A", "int"}, {"B", "A"}});
        const auto noGenerics = cls({});
        const auto single = cls({{"T", ""}});
        accepted(pair, {positional(), positional()}, {"int", "int"});
        accepted(pair, {named("B"), named("A")}, {"int", "int"});
        accepted(pair, {positional(), named("B")}, {"int", "int"});
        accepted(pair, {named("A"), named("B")}, {"int", "int"});
        accepted(single, {named("T")}, {"int"});
        accepted(single, {positional()}, {"int"});
        accepted(defaults, {}, {"int", "int"});
        accepted(defaults, {named("A")}, {"int", "int"});
        accepted(defaults, {named("B")}, {"int", "int"});
        accepted(noGenerics, {}, {});

        const auto triple = cls({{"A", ""}, {"B", ""}, {"C", ""}});
        mapped(pair, {positional(), positional()}, {0, 1});
        mapped(pair, {named("B"), named("A")}, {1, 0});
        mapped(pair, {positional(), named("B")}, {0, 1});
        mapped(triple, {named("C"), named("A"), named("B")}, {2, 0, 1});
        mapped(triple, {positional(), named("C"), named("B")}, {0, 2, 1});
        mapped(defaults, {named("B")}, {1});
        mapped(defaults, {}, {});

        rejected(pair, {named("X"), named("B")}, "unknown named native class generic");
        rejected(pair, {named("a"), named("B")}, "unknown named native class generic");
        rejected(pair, {named("A"), named("A")}, "duplicate native class generic argument");
        rejected(pair, {positional(), named("A")}, "duplicate native class generic argument");
        rejected(pair, {named("B"), positional()}, "positional class generic argument after named");
        rejected(pair, {named("B")}, "cannot infer native class generic parameter");
        rejected(pair, {named("A"), named("B"), named("C")}, "too many native class generic");
        rejected(pair, {named("B", "String"), named("A")}, "supports only int");
        rejected(pair, {named("A", "bool"), named("B")}, "supports only int");
        rejected(pair, {hole("A"), named("B")}, "inference holes require typed constructor");
        rejected(noGenerics, {named("T")}, "non-generic native class cannot accept");
        rejected(cls({{"A", "B"}, {"B", "int"}}), {},
                 "cannot infer native class generic parameter");

        LocalBinding local{};
        local.class_name = "Pair";
        local.class_type_arguments = {named("B"), named("A")};
        local.new_class = true;
        local.constructor_type_arguments = {positional(), named("B")};
        native_verify_local_class_specialization(local, pair);
        ++checks;
        local.constructor_type_arguments = {named("Missing"), named("A")};
        try {
            native_verify_local_class_specialization(local, pair);
            throw std::runtime_error("invalid new-class specialization admitted");
        } catch (const std::runtime_error& ex) {
            if (std::string(ex.what()).find("unknown named native class generic") ==
                    std::string::npos) throw;
            ++checks;
        }
        local.class_name = "Different";
        try {
            native_verify_local_class_specialization(local, pair);
            throw std::runtime_error("wrong nominal owner admitted");
        } catch (const std::runtime_error& ex) {
            if (std::string(ex.what()).find("owner identity mismatch") ==
                    std::string::npos) throw;
            ++checks;
        }
    } catch (const std::exception& e) {
        std::cerr << "native scalar class generic binder FAILED: " << e.what() << '\n';
        return 1;
    }
    if (checks != 32) {
        std::cerr << "native scalar class generic binder count mismatch: " << checks << '\n';
        return 1;
    }
    std::cout << "native scalar class generic binder: 32 host-only cases passed\n";
    return 0;
}
