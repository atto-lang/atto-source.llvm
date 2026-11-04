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
    if (checks != 25) {
        std::cerr << "native scalar class generic binder count mismatch: " << checks << '\n';
        return 1;
    }
    std::cout << "native scalar class generic binder: 25 host-only cases passed\n";
    return 0;
}
