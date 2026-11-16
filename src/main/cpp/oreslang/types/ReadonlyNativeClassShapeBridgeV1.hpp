#pragma once
// Checked native compiler AST class-layout -> readonly structural witness.
// The source vector/offsets come from the *actual* NativeClassLayouts pass.
// This does not authorize LLVM IR emission or convert unrelated class types.
#include "oreslang/types/NativeClassLayout.hpp"
#include "oreslang/frontend/ReadonlyShapeProjectionV1.hpp"

namespace oreslang::frontend::shape_v1 {
inline plan prove_native_class_layout(
        const Program& checked_program,
        const std::string& actual_class_name,
        std::uint32_t checked_nominal_id,
        std::uint64_t checked_layout_epoch,
        std::uint64_t checked_loan_id,
        const std::vector<required_field>& expected) {
    NativeClassLayouts checked_layouts(checked_program);
    const NativeClassLayout& layout = checked_layouts.at(actual_class_name);
    const ClassDecl& original = checked_layouts.source(actual_class_name);

    // Only flat, immutable i64 fields may enter the initial source ABI.
    // Flattened nested owned types need a separate verified borrowed view.
    if (layout.leaves.size() != original.fields.size() ||
        layout.owns_arrays || original.fields.size() > max_fields) {
        throw std::invalid_argument(
            "native structural projection requires a checked flat scalar layout");
    }
    std::vector<field> fields;
    fields.reserve(layout.leaves.size());
    for (std::size_t offset = 0; offset < layout.leaves.size(); ++offset) {
        const NativeClassLeaf& leaf = layout.leaves[offset];
        const ClassField& declaration = original.fields[offset];
        if (leaf.field != &declaration || leaf.path != declaration.name ||
            leaf.access.size() != 1 || declaration.int_array ||
            !declaration.class_name.empty() || !declaration.sum_type.empty()) {
            throw std::invalid_argument(
                "native class source-offset projection is not a single flat i64 field");
        }
        fields.push_back({
            declaration.name,
            declaration.scalar_type == ScalarType::I64 ? scalar::i64 : scalar::boolean,
            static_cast<std::int32_t>(offset),
            declaration.public_member,
            declaration.final_field,
            true, // a compiler-declared scalar field read calls no getter
            false // these ordinary class declarations are not actor state
        });
    }
    return prove(source{origin::class_, checked_nominal_id,
                        checked_layout_epoch, checked_loan_id,
                        false, std::move(fields)}, expected);
}
} // namespace oreslang::frontend::shape_v1
