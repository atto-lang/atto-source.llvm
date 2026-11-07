// Genuine production frontend AST admission, not an experimental source adapter.
// An inverted struct is an explicitly nominal value schema, never a class;
// despite syntactic admission the program cannot pass codegen validation.
#include "oreslang/frontend/Parser.hpp"
#include "oreslang/types/InvertedSchemaMirAdmission.hpp"
#include "oreslang/types/InvertedProductionSourceMir.hpp"
#include "oreslang/types/InvertedMixedGuestLlvm.hpp"

#include <stdexcept>
#include <string>
#include <string_view>

namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class Fn>
void rejects(Fn&& work, std::string_view diagnostic) {
    try { work(); }
    catch (const std::exception& e) {
        if (std::string(e.what()).find(diagnostic) != std::string::npos)
            return;
        throw std::runtime_error(std::string("wrong diagnostic: ") + e.what());
    }
    throw std::runtime_error("unlowered inverted source admitted without failure");
}
}

int main() {
    using namespace oreslang;
    const auto root = parse_unresolved_program(R"ores(
define inverted struct Position as
    let x: int;
    id: i64;
    alive: bool;
    weight: f64;
end
fnc main(): int { return 42; }
)ores");
    check(root.inverted_structs.size() == 1, "missing nominal inverted struct AST");
    const auto& p = root.inverted_structs.front();
    check(p.name == "Position" && !p.exported, "wrong inverted value identity");
    check(p.fields.size() == 4, "inverted field order not preserved");
    check(p.fields[0].name == "x" && p.fields[0].mutable_field &&
          p.fields[0].type == InvertedScalarKind::I64, "let i64 field incorrectly lowered");
    check(p.fields[1].name == "id" && !p.fields[1].mutable_field &&
          p.fields[1].type == InvertedScalarKind::I64, "implicit val must be immutable");
    check(p.fields[2].type == InvertedScalarKind::Bool &&
          p.fields[3].type == InvertedScalarKind::F64,
          "fixed scalar types not retained");
    check(root.classes.empty(), "inverted struct must not become a class");
    // Production parser AST passes through the *same independently verified*
    // MIR schema gate used by resolution. Only a synthetic allocate+close
    // probe is permitted; no source rows or guest codegen are authorized.
    const auto admitted_schemas = dop::mir::preflight_inverted_schemas(root);
    check(admitted_schemas.size() == 1, "production AST MIR preflight lost schema");
    check(admitted_schemas.front().layout.packed_row_bytes() == 25,
          "production schema MIR lost ordered packed field widths");
    check(admitted_schemas.front().probe.instructions().size() == 2,
          "production schema MIR emitted unauthorized guest operations");
    check(admitted_schemas.front().probe.instructions()[1].kind == dop::hir::OpKind::Close,
          "production schema MIR failed to revoke synthetic owner");

    check(root.functions.size() == 1 && root.functions[0].name == "main",
          "following function lost");
    auto unresolved = parse_unresolved_program(
        "define pub inverted struct Public as id: int; end");
    check(unresolved.inverted_structs.size() == 1 &&
          unresolved.inverted_structs[0].exported, "define pub not retained");
    const auto alternate = parse_unresolved_program(
        "pub define inverted struct Public as id: int; end");
    check(alternate.inverted_structs.size() == 1 &&
          alternate.inverted_structs[0].exported, "pub define not retained");
    const auto nested = parse_unresolved_program(R"ores(
define module Mod as
    define inverted struct Point as
        x: int;
        y: int;
    end
end
)ores");
    check(nested.inverted_structs.size() == 1 &&
          nested.inverted_structs[0].name == "Point", "module nested schema lost");

    // The real production lexer recognizes the first executable two-column
    // subset and constructs a typed source operation sequence (no adapter).
    auto live = parse_program(R"ores(
define inverted struct Position as
    let x: int;
    id: int;
end
fnc main(): int {
    let rows = InvertedArray<Position>.with_capacity(3);
    rows.push(11, 100);
    rows.push(20, 101);
    rows.set(1, x, 42);
    rows.erase_swap(0);
    return rows.get(0, x);
}
)ores");
    check(live.functions.empty() && live.inverted_main.has_value(),
          "production parsed inverted main not preserved");
    check(live.inverted_main->schema_name == "Position" &&
          live.inverted_main->owner_name == "rows" &&
          live.inverted_main->capacity == 3,
          "production inverted owner metadata corrupted");
    check(live.inverted_main->operations.size() == 5,
          "production inverted action order corrupted");
    const auto verified = dop::mir::verify_production_inverted_i64(live);
    check(verified.mir.instructions().size() == 7,
          "production source did not produce full MIR and close");
    check(verified.mir.instructions().back().kind == dop::hir::OpKind::Close,
          "production main does not revoke local owner after read");
    const auto ir = dop::mir::emit_native_i64_pair_ir(
        verified.mir, verified.operands);
    check(ir.find("define i32 @main()") != std::string::npos &&
          ir.find("store volatile i1 false, ptr %alive") != std::string::npos &&
          ir.find("call void @llvm.trap()") != std::string::npos,
          "production verified MIR failed to emit checked native IR");

    rejects([] {
        (void)parse_program(R"ores(
define inverted struct P as let x: int; id: int; end
fnc main(): int {
    let rows = InvertedArray<P>.with_capacity(2);
    rows.push(11, 22);
    rows.set(0, id, 42);
    return rows.get(0, x);
}
)ores");
    }, "immutable inverted field cannot be written");
    rejects([] {
        (void)parse_program(R"ores(
define inverted struct P as let x: int; id: int; end
fnc main(): int {
    let rows = InvertedArray<P>.with_capacity(2);
    rows.push(11, 22);
    rows.erase_swap(0);
    return rows.get(0, x);
}
fnc hidden(): int { return 0; }
)ores");
    }, "only source callable");
    rejects([] {
        (void)parse_program(R"ores(
define inverted struct P as let x: int; id: int; end
fnc main(): int {
    let rows = InvertedArray<P>.with_capacity(2);
    rows.push(1, 2);
    rows.erase_swap(1);
    return rows.get(0, x);
}
)ores");
    }, "outside current length");

    // Real Parser.cpp -> Program -> resolve_and_validate_program must not
    // silently discard extra declaration categories that this narrow native
    // MIR/stack-owner implementation does not know how to lower.
    const auto fresh_inverted_main = []() {
        return parse_unresolved_program(R"ores(
define inverted struct Position as let x: int; id: int; end
fnc main(): int {
    let rows = InvertedArray<Position>.with_capacity(2);
    rows.push(11, 21);
    return rows.get(0, x);
}
)ores");
    };
    rejects([&] {
        auto poisoned = fresh_inverted_main();
        poisoned.sum_types.push_back(NativeSumType{});
        resolve_and_validate_program(poisoned);
    }, "requires one standalone local main");
    rejects([&] {
        auto poisoned = fresh_inverted_main();
        poisoned.conforms.push_back("UnloweredCapability");
        resolve_and_validate_program(poisoned);
    }, "requires one standalone local main");
    rejects([&] {
        auto poisoned = fresh_inverted_main();
        poisoned.type_constraint_descriptors.push_back(
            TypeConstraintDescriptor{});
        resolve_and_validate_program(poisoned);
    }, "requires one standalone local main");

    rejects([] {
        auto p = parse_unresolved_program(
            "define inverted struct Point as x: int; end");
        resolve_and_validate_program(p);
    }, "typed SoA HIR/MIR ownership lowering is not implemented");
    rejects([] {
        (void)parse_program("define inverted struct Point as x: int; end");
    }, "typed SoA HIR/MIR ownership lowering is not implemented");
    rejects([] {
        (void)parse_unresolved_program(
            "define inverted struct P as x: int; x: bool; end");
    }, "duplicate inverted struct field");
    rejects([] {
        (void)parse_unresolved_program(
            "define inverted struct P as x: Pointer<int>; end");
    }, "inverted struct member requires fixed scalar");
    rejects([] {
        (void)parse_unresolved_program(
            "define inverted struct P as x: Other; end");
    }, "inverted struct member requires fixed scalar");
    rejects([] {
        (void)parse_unresolved_program(
            "define inverted struct P as x: int; end\n"
            "define inverted struct P as y: int; end");
    }, "duplicate inverted struct declaration");
    rejects([] {
        (void)parse_unresolved_program(
            "define inverted struct Empty as end");
    }, "empty inverted struct");
    rejects([] {
        (void)parse_unresolved_program(
            "define inverted class P as x: int; end");
    }, "native inverted class requires typed one-level SoA ownership");
    rejects([] {
        (void)parse_unresolved_program(
            "define inverted struct P as x: int = 1; end");
    }, "expected ';'");

    // Real production lexer/source AST -> checked mixed MIR -> LLVM IR.
    // The real guest process execution is separately required by CTest.
    const auto mixed_guest = parse_program(R"atto(
define inverted struct Particle as
    let x: int;
    alive: bool;
    let energy: f64;
end
fnc main(): int {
    let rows = InvertedArray<Particle>.with_capacity(3);
    rows.push(11, true, 1.5);
    rows.push(17, false, -2.25);
    rows.set(1, energy, 3.75);
    rows.set(1, x, 42);
    rows.erase_swap(0);
    return rows.get(0, x);
}
)atto");
    check(mixed_guest.inverted_main.has_value() &&
          mixed_guest.inverted_main->three_scalar &&
          mixed_guest.inverted_structs.size() == 1 &&
          mixed_guest.inverted_structs.front().fields.size() == 3,
          "production typed mixed main missing from AST");
    const auto verified_mixed = dop::mir::verify_production_inverted_mixed(mixed_guest);
    check(verified_mixed.mir.instructions().size() == 8 &&
          verified_mixed.mir.instructions().back().kind == dop::hir::OpKind::Close,
          "mixed source failed independent typed MIR/owner close");
    const auto mixed_ir = dop::mir::emit_mixed_source_ir(verified_mixed);
    check(mixed_ir.find("alloca [3 x i64]") != std::string::npos &&
          mixed_ir.find("alloca [3 x i1]") != std::string::npos &&
          mixed_ir.find("alloca [3 x double]") != std::string::npos &&
          mixed_ir.find("0x400E000000000000") != std::string::npos &&
          mixed_ir.find("store volatile double") != std::string::npos,
          "production AST mixed MIR lost typed LLVM column lowering");
    rejects([] {
        (void)parse_program(R"atto(
define inverted struct P as x: int; active: bool; f: f64; end
fnc main(): int {
    let rows = InvertedArray<P>.with_capacity(2);
    rows.push(1, 1, 1.5);
    return rows.get(0, x);
}
)atto");
    }, "mixed inverted bool column");
    rejects([] {
        (void)parse_program(R"atto(
define inverted struct P as x: int; active: bool; f: f64; end
fnc main(): int {
    let rows = InvertedArray<P>.with_capacity(2);
    rows.push(1, true, 1.5);
    return rows.get(0, f);
}
)atto");
    }, "mixed inverted int main");
    rejects([] {
        (void)parse_program("fnc main(): int { return 1.5; }");
    }, "native floating-point/decimal literal unsupported");

    // Full production source parser (not synthetic MIR operands) must retain
    // comparison type, literal and both integer branches after checked read.
    const auto f64_comparison = parse_program(R"atto(
define inverted struct Particle as
    let x: int;
    alive: bool;
    let energy: f64;
end
fnc main(): int {
    let rows = InvertedArray<Particle>.with_capacity(2);
    rows.push(7, true, 1.5);
    rows.push(19, false, 3.75);
    rows.erase_swap(0);
    return rows.get(0, energy) == 3.75 ? 42 : 7;
}
)atto");
    check(f64_comparison.inverted_main.has_value() &&
          f64_comparison.inverted_main->operations.back().kind ==
              InvertedSourceOperation::Kind::ReturnCompareF64 &&
          f64_comparison.inverted_main->operations.back().equal_result == 42 &&
          f64_comparison.inverted_main->operations.back().unequal_result == 7,
          "typed source f64 equality and return branches not retained");
    const auto f64_verified = dop::mir::verify_production_inverted_mixed(f64_comparison);
    const auto f64_ir = dop::mir::emit_mixed_source_ir(f64_verified);
    check(f64_ir.find(" = load double, ptr ") != std::string::npos &&
          f64_ir.find(" = fcmp oeq double ") != std::string::npos &&
          f64_ir.find(" = select i1 ") != std::string::npos,
          "native compiler failed to lower f64 runtime comparison");

    rejects([] {
        (void)parse_program(R"atto(
define inverted struct P as x: int; active: bool; f: f64; end
fnc main(): int {
    let rows = InvertedArray<P>.with_capacity(2);
    rows.push(7, true, 3.75);
    return rows.get(0, f);
}
)atto");
    }, "requires explicit f64 equality");
    rejects([] {
        (void)parse_program(R"atto(
define inverted struct P as x: int; active: bool; f: f64; end
fnc main(): int {
    let rows = InvertedArray<P>.with_capacity(2);
    rows.push(7, true, 3.75);
    return rows.get(0, f) == 3 ? 42 : 7;
}
)atto");
    }, "mixed inverted f64 column requires a real literal");
    return 0;
}
