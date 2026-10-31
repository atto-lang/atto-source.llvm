// Strict C++17 test of production Program AST -> independently verified
// *schema-only* MIR preflight. Does not compile or authorize guest operations.
#include "oreslang/types/InvertedSchemaMirAdmission.hpp"
#include <cassert>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
template<class F> void rejects(F&& fn, const char* expected) {
    try { fn(); } catch (const std::exception& error) {
        if (std::string(error.what()).find(expected) != std::string::npos)
            return;
        throw std::runtime_error(std::string("unexpected schema-MIR diagnostic: ") + error.what());
    }
    throw std::runtime_error(std::string("missing schema-MIR rejection: ") + expected);
}
oreslang::InvertedStructDecl position() {
    using namespace oreslang;
    InvertedStructDecl d;
    d.name = "Position";
    d.fields = {
        InvertedStructField{"x", InvertedScalarKind::I64, true, 0},
        InvertedStructField{"id", InvertedScalarKind::I64, false, 0},
        InvertedStructField{"alive", InvertedScalarKind::Bool, false, 0}
    };
    return d;
}
}
int main() {
    using namespace oreslang;
    using namespace oreslang::dop::hir;
    using namespace oreslang::dop::mir;
    Program program;
    program.inverted_structs.push_back(position());
    auto schemas = preflight_inverted_schemas(program);
    assert(schemas.size() == 1);
    assert(schemas[0].layout.name() == "Position");
    assert(schemas[0].layout.columns() == 3);
    assert(schemas[0].layout.packed_row_bytes() == 17);
    assert(schemas[0].layout.column(0).mutable_field);
    assert(!schemas[0].layout.column(1).mutable_field);
    const auto& probe = schemas[0].probe.instructions();
    assert(probe.size() == 2);
    assert(probe[0].kind == OpKind::Allocate);
    assert(probe[1].kind == OpKind::Close);
    assert(probe[0].index == 1); // synthetic probe, NOT source capacity
    assert(probe[0].require_owner_guard == false);
    assert(probe[1].require_owner_guard);

    Program duplicate = program;
    duplicate.inverted_structs.push_back(position());
    rejects([&]{ (void)preflight_inverted_schemas(duplicate); },
            "duplicate nominal inverted struct identity");

    Program collision = program;
    ClassDecl cls; cls.name = "Position";
    collision.classes.push_back(std::move(cls));
    rejects([&]{ (void)preflight_inverted_schemas(collision); },
            "collides with class");
    collision.classes.clear();
    ActorDecl actor; actor.name = "Position";
    collision.actors.push_back(std::move(actor));
    rejects([&]{ (void)preflight_inverted_schemas(collision); },
            "collides with actor");
    collision.actors.clear();
    TypeOnlyDecl alias; alias.name = "Position";
    collision.type_only_declarations.push_back(std::move(alias));
    rejects([&]{ (void)preflight_inverted_schemas(collision); },
            "collides with type-only");

    Program bad = program;
    bad.inverted_structs[0].fields[2].type =
        static_cast<InvertedScalarKind>(99);
    rejects([&]{ (void)preflight_inverted_schemas(bad); },
            "unsupported inverted scalar field");
    Program over_budget;
    for (std::size_t i = 0; i < 65; ++i) {
        auto schema = position();
        schema.name += std::to_string(i);
        over_budget.inverted_structs.push_back(std::move(schema));
    }
    rejects([&]{ (void)preflight_inverted_schemas(over_budget); },
            "declaration budget exceeded");
    assert(preflight_inverted_schemas(Program{}).empty());
    return 0;
}
