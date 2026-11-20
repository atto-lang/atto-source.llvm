#include "oreslang/types/NativeAssociatedWitnessContract.hpp"

#include <cassert>
#include <stdexcept>
#include <string>
#include <vector>

using namespace oreslang::types;
namespace {
AssociatedTypeTerm named(std::string module, std::string name,
        std::vector<AssociatedTypeTerm> args = {}) {
    return AssociatedTypeTerm::named(std::move(module), std::move(name), std::move(args));
}
AssociatedTypeTerm var(std::string name) {
    return AssociatedTypeTerm::var(std::move(name));
}
template<typename F>
void rejected(F&& action) {
    bool failed = false;
    try {
        action();
    } catch (const std::invalid_argument&) {
        failed = true;
    }
    assert(failed && "static witness admission must reject invalid proof");
}
} // namespace

int main() {
    {
        NativeAssociatedWitnessRegistry reg;
        reg.define("std", "IteratorLike", 1, {"item", "error"});
        reg.register_witness({"std", "IteratorLike", "std", "std.list_iterator",
            {named("core", "List", {var("T")})},
            {{"item", var("T")}, {"error", named("core", "ReadError", {var("T")})}}});
        const auto result = reg.resolve("std", "IteratorLike",
                {named("core", "List", {named("core", "int")})});
        assert(result.dictionary_symbol == "std.list_iterator");
        assert(result.equations.at("item") == named("core", "int"));
        assert(result.equations.at("error") ==
                named("core", "ReadError", {named("core", "int")}));
        rejected([&] { (void)reg.resolve("std", "IteratorLike", {named("core", "bool")}); });
        rejected([&] { (void)reg.resolve("std", "IteratorLike", {var("X")}); });
    }
    {
        NativeAssociatedWitnessRegistry reg;
        reg.define("std", "Decode", 1, {"decoded", "error"});
        auto witness = AssociatedWitness{"std", "Decode", "std", "decode_blob",
            {named("core", "Blob", {var("T")})}, {{"decoded", var("T")}}};
        rejected([&] { reg.register_witness(witness); });
        witness.equations["error"] = var("Unbound");
        rejected([&] { reg.register_witness(witness); });
        witness.equations["error"] = named("core", "Error");
        reg.register_witness(witness);
        assert(reg.resolve("std", "Decode",
                {named("core", "Blob", {named("core", "int")})})
                .equations.at("decoded") == named("core", "int"));
    }
    {
        NativeAssociatedWitnessRegistry reg;
        reg.define("logic", "Same", 2, {"item"});
        reg.register_witness({"logic", "Same", "logic", "logic.same",
                {var("T"), var("T")}, {{"item", var("T")}}});
        assert(reg.resolve("logic", "Same",
                {named("core", "int"), named("core", "int")}).equations.at("item")
                == named("core", "int"));
        rejected([&] { (void)reg.resolve("logic", "Same",
                {named("core", "int"), named("core", "bool")}); });
        rejected([&] { reg.register_witness({"logic", "Same", "logic",
                "logic.same_int", {named("core", "int"), named("core", "int")},
                {{"item", named("core", "bool")}}}); });
    }
    {
        NativeAssociatedWitnessRegistry reg;
        reg.define("meta", "ItemOf", 1, {"item"});
        reg.register_witness({"meta", "ItemOf", "meta", "meta.i64",
                {named("core", "int")}, {{"item", named("core", "int")}}});
        rejected([&] { reg.register_witness({"meta", "ItemOf", "meta", "meta.i64",
                {named("core", "bool")}, {{"item", named("core", "bool")}}}); });
        rejected([&] { reg.register_witness({"meta", "ItemOf", "foreign", "foreign.bool",
                {named("core", "bool")}, {{"item", named("core", "bool")}}}); });
        reg.register_witness({"meta", "ItemOf", "meta", "meta.bool",
                {named("core", "bool")}, {{"item", named("core", "bool")}}});
        assert(reg.resolve("meta", "ItemOf",
                {named("core", "bool")}).dictionary_symbol == "meta.bool");
    }
    {
        NativeAssociatedWitnessRegistry first, second;
        for (auto* registry : {&first, &second}) registry->define("types", "Coll", 1, {"item"});
        auto integer = AssociatedWitness{"types", "Coll", "types", "types.int",
                {named("core", "int")}, {{"item", named("core", "int")}}};
        auto boolean = AssociatedWitness{"types", "Coll", "types", "types.bool",
                {named("core", "bool")}, {{"item", named("core", "bool")}}};
        first.register_witness(integer);
        first.register_witness(boolean);
        second.register_witness(boolean);
        second.register_witness(integer);
        for (auto type : {named("core", "int"), named("core", "bool")}) {
            assert(first.resolve("types", "Coll", {type}).equations ==
                    second.resolve("types", "Coll", {type}).equations);
        }
    }
    {
        NativeAssociatedWitnessRegistry collisionSafe;
        // Ambiguous if protocol identity is concatenated with raw "::".
        collisionSafe.define("a::b", "c", 1, {"item"});
        collisionSafe.define("a", "b::c", 1, {"item"});
        collisionSafe.register_witness({"a::b", "c", "a::b", "first",
                {named("core", "int")}, {{"item", named("core", "int")}}});
        collisionSafe.register_witness({"a", "b::c", "a", "second",
                {named("core", "bool")}, {{"item", named("core", "bool")}}});
        assert(collisionSafe.resolve("a::b", "c", {named("core", "int")})
                .dictionary_symbol == "first");
        assert(collisionSafe.resolve("a", "b::c", {named("core", "bool")})
                .dictionary_symbol == "second");
        rejected([&] { (void)collisionSafe.resolve("a::b", "c",
                {named("core", "bool")}); });
    }
    {
        // Distinct nominal *type* identities must not alias at the
        // witness-overlap stage even if their raw "::" display strings do.
        NativeAssociatedWitnessRegistry nominal;
        nominal.define("meta", "DistinctNominal", 1, {"item"});
        const auto first = named("a::b", "c");
        const auto second = named("a", "b::c");
        nominal.register_witness({"meta", "DistinctNominal", "meta", "meta.first",
                {first}, {{"item", first}}});
        nominal.register_witness({"meta", "DistinctNominal", "meta", "meta.second",
                {second}, {{"item", second}}});
        assert(nominal.resolve("meta", "DistinctNominal", {first})
                .equations.at("item") == first);
        assert(nominal.resolve("meta", "DistinctNominal", {second})
                .equations.at("item") == second);
    }
    return 0;
}
