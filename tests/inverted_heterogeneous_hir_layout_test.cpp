// Compiler HIR admission regression; no actual .atto guest or LLVM IR runs here.
#include "oreslang/types/InvertedTypedPlan.hpp"
#include <stdexcept>
#include <string>
#include <iostream>

using oreslang::InvertedScalarKind;
using oreslang::InvertedStructDecl;
using oreslang::InvertedStructField;
using oreslang::dop::hir::Layout;

static void require(bool ok, const char* reason) {
    if (!ok) throw std::runtime_error(reason);
}

template<class F> static void reject(F callback) {
    try { callback(); } catch (const std::exception&) { return; }
    throw std::runtime_error("invalid compiler-owned inverted layout was admitted");
}

int main() {
    InvertedStructDecl declaration;
    declaration.name = "Mixed";
    declaration.fields = {
        InvertedStructField{"flag", InvertedScalarKind::Bool, true, 0},
        InvertedStructField{"ticks", InvertedScalarKind::I64, false, 0},
        InvertedStructField{"alive", InvertedScalarKind::Bool, true, 0},
        InvertedStructField{"weight", InvertedScalarKind::F64, true, 0}
    };
    const auto layout = Layout::from_decl(declaration);
    require(layout.packed_row_bytes() == 18, "packed nominal width is not scalar sum");
    require(layout.column(0).packed_row_offset == 0 &&
            layout.column(1).packed_row_offset == 1 &&
            layout.column(2).packed_row_offset == 9 &&
            layout.column(3).packed_row_offset == 10,
            "nominal packed row offsets changed");
    const auto physical = layout.physical_soa(2);
    require(physical.columns() == 4 && physical.bytes() == 48,
            "inter-column alignment was not charged");
    require(physical.column_base(0) == 0 && physical.column_base(1) == 8 &&
            physical.column_base(2) == 24 && physical.column_base(3) == 32,
            "compiler HIR assigned wrong physical field offsets");
    require(layout.capacity_bytes(2) == 48, "HIR allocation admits undercharged bytes");
    require(physical.offset(3,1) == 40, "f64 row stride changed");

    // Compile-time typed loans retain independent field granularity.
    oreslang::dop::hir::LinearPlan plan(layout, 2);
    plan.push(4);
    plan.borrow_read("ticks");
    plan.borrow_write("alive");
    reject([&]{ plan.borrow_read("alive"); });
    reject([&]{ plan.erase_swap(0); });
    plan.release_write("alive");
    plan.release_read("ticks");
    plan.erase_swap(0);
    plan.close();
    require(plan.closed(), "compiler HIR owner did not close");

    // Exact alignment-overflow boundary: the old size*capacity check returned
    // 16,777,210 bytes. A physical bool/i64/bool SoA needs 16,777,217 bytes,
    // which exceeds the 16 MiB owner budget by one byte.
    InvertedStructDecl edge;
    edge.name = "Edge";
    edge.fields = {
        InvertedStructField{"a", InvertedScalarKind::Bool, true, 0},
        InvertedStructField{"b", InvertedScalarKind::I64, true, 0},
        InvertedStructField{"c", InvertedScalarKind::Bool, true, 0},
    };
    const auto boundary = Layout::from_decl(edge);
    const std::size_t capacity = (16u * 1024u * 1024u) / 10;
    require(capacity == 1677721, "unexpected test boundary");
    reject([&]{ (void)boundary.capacity_bytes(capacity); });
    reject([&]{ oreslang::dop::hir::LinearPlan forbidden(boundary, capacity); });
    require(boundary.capacity_bytes(capacity - 1) <= 16u * 1024u * 1024u,
            "safe size below boundary rejected");

    std::cout << "INVERTED_HETEROGENEOUS_HIR_HOST_PASS groups=3" << std::endl;
}
