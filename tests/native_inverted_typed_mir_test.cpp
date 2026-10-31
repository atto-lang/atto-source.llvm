// MIR verifier and actual native InvertedColumns execution, not a mocked ABI.
// This does not certify source-to-MIR lowering or the full oreslang-llvmc.
#include "oreslang/types/InvertedTypedMir.hpp"
#include <cassert>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
template<typename Fn> void reject(Fn&& fn, const char* reason) {
    try { fn(); }
    catch (const std::exception& e) {
        if (std::string(e.what()).find(reason) != std::string::npos) return;
        throw std::runtime_error(std::string("wrong MIR rejection: ") + e.what());
    }
    throw std::runtime_error(std::string("missing MIR rejection: ") + reason);
}
}

int main() {
    using namespace oreslang;
    using namespace oreslang::dop::hir;
    using namespace oreslang::dop::mir;
    InvertedStructDecl decl;
    decl.name = "Pair";
    decl.fields = {
        InvertedStructField{"x", InvertedScalarKind::I64, true, 0},
        InvertedStructField{"id", InvertedScalarKind::I64, false, 0}
    };
    const auto schema = Layout::from_decl(decl);
    LinearPlan plan(schema, 3);
    plan.push(2);
    plan.push(2);
    plan.write_runtime_checked(0, "x");
    plan.read_runtime_checked(0, "id");
    plan.borrow_read("id");
    plan.borrow_write("x");
    plan.release_write("x");
    plan.release_read("id");
    plan.snapshot(1);
    plan.erase_swap(0);
    plan.read_runtime_checked(0, "x");
    const auto module = Module::lower(plan);
    assert(module.instructions().size() == 12);
    assert(module.capacity() == 3);
    const auto result = execute_native_i64_pair(module,
        I64PairInputs{{{11, 501}, {21, 502}}, {42}});
    assert(result.live_rows == 1);
    assert(result.reads == std::vector<std::int64_t>({501, 21}));
    assert(result.snapshots.size() == 1);
    assert(result.snapshots[0] == (std::array<std::int64_t,2>{21, 502}));

    reject([&] { plan.write_runtime_checked(0, "id"); },
           "immutable inverted field");
    reject([&] { plan.suspend(); }, "cannot cross suspension");

    auto forged = module.instructions();
    forged[3].require_bounds_guard = false;
    reject([&] { (void)Module::verify_external(schema, 3, forged); },
           "mandatory owner/generation/bounds guard missing");

    forged = module.instructions();
    forged[3].column = 1; // convert legal x write into illegal id write
    reject([&] { (void)Module::verify_external(schema, 3, forged); },
           "immutable or borrowed field write");

    forged = module.instructions();
    forged[6].kind = OpKind::Close; // with live read borrow
    forged[6].column = schema.columns();
    forged[6].require_bounds_guard = false;
    forged[6].require_generation_guard = false;
    reject([&] { (void)Module::verify_external(schema, 3, forged); },
           "close with live loans");

    forged = module.instructions();
    forged[0].index = 999;
    reject([&] { (void)Module::verify_external(schema, 3, forged); },
           "one exact allocation");

    LinearPlan runtime_bounds(schema, 1);
    runtime_bounds.push(2);
    runtime_bounds.read_runtime_checked(99, "x");
    const auto runtime_mir = Module::lower(runtime_bounds);
    reject([&] {
        (void)execute_native_i64_pair(runtime_mir,
            I64PairInputs{{{8,88}}, {}});
    }, "outside current length");

    forged = module.instructions();
    forged.back().index = std::numeric_limits<std::size_t>::max();
    reject([&] { (void)Module::verify_external(schema, 3, forged); },
           "runtime index exceeds signed i64");

    // All values returned by snapshot are detached from the owner: closing
    // the native store cannot invalidate the previously copied result.
    LinearPlan closed(schema, 1);
    closed.push(2);
    closed.snapshot(0);
    closed.close();
    const auto closed_result = execute_native_i64_pair(
        Module::lower(closed), I64PairInputs{{{7, 101}}, {}});
    assert(closed_result.live_rows == 0);
    assert(closed_result.snapshots.size() == 1);
    assert(closed_result.snapshots[0] ==
           (std::array<std::int64_t, 2>{7, 101}));
    reject([&] { closed.read_runtime_checked(0, "x"); },
           "closed inverted HIR owner");

    // Two independent logical readers share the lane's checked native read
    // lease. The second release must not attempt a second native release.
    LinearPlan readers(schema, 1);
    readers.push(2);
    readers.borrow_read("id");
    readers.borrow_read("id");
    readers.release_read("id");
    readers.release_read("id");
    readers.snapshot(0);
    const auto read_result = execute_native_i64_pair(
        Module::lower(readers), I64PairInputs{{{31, 503}}, {}});
    assert(read_result.snapshots.size() == 1);
    assert(read_result.snapshots[0][1] == 503);

    LinearPlan no_owner_pin(schema, 1);
    no_owner_pin.push(2);
    reject([&] { no_owner_pin.suspend(); }, "cannot cross suspension");
    return 0;
}
