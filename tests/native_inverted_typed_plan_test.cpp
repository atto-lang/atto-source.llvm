// No LLVM SDK needed: exercise typed HIR admission and the REAL checked
// InvertedColumns native owner, not a simulation or source-to-guest claim.
#include "oreslang/types/InvertedTypedPlan.hpp"
#include "oreslang/runtime/DopInvertedColumns.hpp"

#include <cassert>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace {
template<class F>
void rejects(F&& fn, std::string_view diagnostic) {
    try { fn(); }
    catch (const std::exception& e) {
        if (std::string_view(e.what()).find(diagnostic) != std::string_view::npos)
            return;
        throw std::runtime_error(std::string("incorrect typed HIR diagnostic: ") + e.what());
    }
    throw std::runtime_error(std::string("expected typed HIR rejection: ") + std::string(diagnostic));
}
oreslang::InvertedStructDecl sample() {
    using namespace oreslang;
    InvertedStructDecl decl;
    decl.name = "Position";
    decl.fields.push_back(InvertedStructField{
        "x", InvertedScalarKind::I64, true, 0});
    decl.fields.push_back(InvertedStructField{
        "id", InvertedScalarKind::I64, false, 0});
    decl.fields.push_back(InvertedStructField{
        "alive", InvertedScalarKind::Bool, true, 0});
    decl.fields.push_back(InvertedStructField{
        "weight", InvertedScalarKind::F64, true, 0});
    return decl;
}
}

int main() {
    using namespace oreslang;
    using namespace oreslang::dop;
    using namespace oreslang::dop::hir;
    auto schema = Layout::from_decl(sample());
    assert(schema.name() == "Position");
    assert(schema.columns() == 4);
    assert(schema.packed_row_bytes() == 25);
    assert(schema.column(0).packed_row_offset == 0);
    assert(schema.column(1).packed_row_offset == 8);
    assert(schema.column(2).packed_row_offset == 16);
    assert(schema.column(3).packed_row_offset == 17);
    assert(schema.column(2).element_bytes == 1);
    assert(!schema.column(1).mutable_field && schema.column(0).mutable_field);
    assert(schema.field_index("weight") == 3);
    assert(schema.capacity_bytes(100) == 2500);
    rejects([&] { (void)schema.capacity_bytes(0); }, "must be positive");
    rejects([&] { (void)schema.capacity_bytes(1000000); }, "16 MiB");
    rejects([&] { (void)schema.field_index("forgotten"); }, "unknown inverted field");

    auto invalid = sample();
    invalid.fields[1].name = "x";
    rejects([&] { (void)Layout::from_decl(invalid); }, "duplicate inverted field");
    invalid = sample();
    invalid.fields[2].type = static_cast<InvertedScalarKind>(255);
    rejects([&] { (void)Layout::from_decl(invalid); }, "unsupported inverted scalar");
    invalid = sample();
    invalid.fields.clear();
    rejects([&] { (void)Layout::from_decl(invalid); }, "1..64 columns");

    LinearPlan plan(schema, 3);
    assert(plan.operations().size() == 1);
    assert(plan.operations()[0].kind == OpKind::Allocate);
    rejects([&] { plan.push(3); }, "arity mismatch");
    plan.push(4); plan.push(4);
    assert(plan.length() == 2);
    assert(plan.read(1, "id") == 1);
    rejects([&] { plan.write(1, "id"); }, "immutable inverted field");
    assert(plan.write(1, "x") == 0);
    rejects([&] { plan.read(2, "x"); }, "outside current length");
    plan.borrow_read("id");
    plan.borrow_read("id");
    plan.borrow_write("x"); // different field admitted
    rejects([&] { plan.borrow_write("id"); }, "immutable inverted field");
    rejects([&] { plan.write(0, "x"); }, "outstanding loan");
    rejects([&] { plan.borrow_read("x"); }, "already mutably borrowed");
    rejects([&] { plan.snapshot(0); }, "mutable loan");
    rejects([&] { plan.push(4); }, "structural operation conflicts");
    rejects([&] { plan.erase_swap(0); }, "structural operation conflicts");
    rejects([&] { plan.suspend(); }, "cannot cross suspension");
    rejects([&] { plan.close(); }, "structural operation conflicts");
    plan.release_write("x");
    plan.snapshot(0); // immutable read loans do not prevent detached snapshot
    rejects([&] { plan.release_write("x"); }, "unmatched inverted write release");
    plan.release_read("id");
    rejects([&] { plan.erase_swap(1); }, "structural operation conflicts");
    plan.release_read("id");
    rejects([&] { plan.release_read("id"); }, "unmatched inverted read release");
    rejects([&] { plan.suspend(); }, "cannot cross suspension"); // pinning unproven
    plan.erase_swap(0);
    assert(plan.length() == 1);
    plan.push(4);
    assert(plan.length() == 2);
    plan.close();
    assert(plan.closed() && plan.length() == 0);
    rejects([&] { plan.push(4); }, "closed inverted HIR owner");
    rejects([&] { plan.read(0, "x"); }, "closed inverted HIR owner");
    rejects([&] { plan.close(); }, "closed inverted HIR owner");

    LinearPlan quota(schema, 1);
    quota.push(4);
    rejects([&] { quota.push(4); }, "capacity exceeded");

    // Cross-check the actual native owner semantics separately. Compile-time
    // HIR mutability rules are stronger than this untyped host API. Call only
    // source-admitted operations (and deliberately test stale handle guards).
    InvertedColumns<std::int64_t, std::int64_t> store;
    store.reserve(2);
    auto first = store.push(11, 7);
    auto second = store.push(42, 8);
    assert(store.size() == 2);
    assert(store.read_index<0>(1) == 42);
    store.write_index<0>(0, 99);
    assert(store.get<0>(first) == 99);
    {
        auto reader = store.borrow_read<1>();
        auto writer = store.borrow_write<0>();
        assert(reader.get(0) == 7);
        writer.set(1, 43);
        assert(writer.get(1) == 43);
        rejects([&] { store.erase_swap(first); }, "cannot erase borrowed");
        rejects([&] { store.push(1, 1); }, "cannot grow borrowed");
    }
    assert(store.get<0>(second) == 43);
    store.erase_swap(first);
    assert(store.size() == 1);
    assert(store.index(0).slot() == second.slot());
    rejects([&] { (void)store.get<0>(first); }, "stale inverted row handle");
    rejects([&] { (void)store.index(1); }, "outside current length");
    auto reused = store.push(12, 13);
    assert(reused.slot() == first.slot());
    assert(reused.generation() != first.generation());
    rejects([&] { (void)store.get<0>(first); }, "stale inverted row handle");
    assert(std::get<0>(store.get_row(second)) == 43);

    InvertedColumns<std::int64_t, std::int64_t> foreign;
    auto unrelated = foreign.push(1, 2);
    rejects([&] { (void)store.get<0>(unrelated); }, "foreign or invalid");
    return 0;
}
