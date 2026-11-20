#pragma once
// Production Program AST -> typed, verified schema-only MIR preflight.
// A schema probe deliberately has no guest operations, source rows or IR
// lowering authority. Never treat this as source -> executable MIR.
#include "oreslang/types/InvertedTypedMir.hpp"

#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace oreslang::dop::mir {

struct InvertedSchemaPreflight {
    hir::Layout layout;
    Module probe;
};

inline std::vector<InvertedSchemaPreflight> preflight_inverted_schemas(
        const Program& program) {
    if (program.inverted_structs.size() > 64)
        throw std::invalid_argument("native inverted schema declaration budget exceeded");
    std::unordered_set<std::string> names;
    std::vector<InvertedSchemaPreflight> result;
    result.reserve(program.inverted_structs.size());
    for (const auto& decl : program.inverted_structs) {
        auto layout = hir::Layout::from_decl(decl);
        if (!names.insert(layout.name()).second)
            throw std::invalid_argument("duplicate nominal inverted struct identity");
        for (const auto& cls : program.classes)
            if (cls.name == layout.name())
                throw std::invalid_argument("inverted struct nominal collides with class declaration");
        for (const auto& actor : program.actors)
            if (actor.name == layout.name())
                throw std::invalid_argument("inverted struct nominal collides with actor declaration");
        for (const auto& type_only : program.type_only_declarations)
            if (type_only.name == layout.name())
                throw std::invalid_argument("inverted struct nominal collides with type-only declaration");

        // Capacity 1 is a synthetic schema-only verifier probe, NOT
        // authorization for any guest owner allocation or row access. Actual
        // source capacity must be separately admitted and checked.
        hir::LinearPlan schema_probe(layout, 1);
        schema_probe.close();
        auto verified = Module::lower(schema_probe);
        if (verified.instructions().size() != 2 ||
            verified.instructions()[0].kind != hir::OpKind::Allocate ||
            verified.instructions()[1].kind != hir::OpKind::Close)
            throw std::logic_error("inverted schema probe leaked guest operations");
        result.push_back(InvertedSchemaPreflight{
            std::move(layout), std::move(verified)});
    }
    return result;
}

} // namespace oreslang::dop::mir
