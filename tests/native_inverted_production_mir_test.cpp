// Standalone strict C++17 admission test: production Program action AST,
// independently checked HIR/MIR and guarded LLVM IR. No LLVM SDK required.
// This does not replace production Parser.cpp or executable native guest tests.
#include "oreslang/types/InvertedProductionSourceMir.hpp"

#include <cassert>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
using oreslang::InvertedSourceOperation;
using oreslang::InvertedSourceMain;
using oreslang::InvertedScalarKind;
using oreslang::InvertedStructDecl;
using oreslang::InvertedStructField;
using oreslang::Program;

Program make_source() {
    Program p;
    InvertedStructDecl decl;
    decl.name = "Position";
    decl.fields = {
        InvertedStructField{"x", InvertedScalarKind::I64, true, 0},
        InvertedStructField{"id", InvertedScalarKind::I64, false, 0}
    };
    p.inverted_structs.push_back(std::move(decl));
    InvertedSourceMain body;
    body.schema_name = "Position";
    body.owner_name = "rows";
    body.capacity = 3;
    using Kind = InvertedSourceOperation::Kind;
    body.operations = {
        InvertedSourceOperation{Kind::Push, 0, "", {{11, 100}}, 0},
        InvertedSourceOperation{Kind::Push, 0, "", {{20, 101}}, 0},
        InvertedSourceOperation{Kind::WriteField, 1, "x", {}, 42},
        InvertedSourceOperation{Kind::EraseSwap, 0, "", {}, 0},
        InvertedSourceOperation{Kind::ReturnRead, 0, "x", {}, 0}
    };
    p.inverted_main = std::move(body);
    return p;
}

template<class F> void reject(F&& fn, const char* expected) {
    try { fn(); }
    catch (const std::exception& e) {
        if (std::string(e.what()).find(expected) != std::string::npos)
            return;
        throw std::runtime_error(std::string("wrong production-MIR diagnostic: ") + e.what());
    }
    throw std::runtime_error(std::string("forged production-MIR operation admitted: ") + expected);
}
}

int main() {
    using namespace oreslang::dop::mir;
    using oreslang::dop::hir::OpKind;
    const auto valid = make_source();
    const auto compiled = verify_production_inverted_i64(valid);
    const auto& ops = compiled.mir.instructions();
    assert(ops.size() == 7);
    assert(ops.front().kind == OpKind::Allocate);
    assert(ops[1].kind == OpKind::Push && ops[2].kind == OpKind::Push);
    assert(ops[3].kind == OpKind::WriteField);
    assert(ops[4].kind == OpKind::EraseSwap);
    assert(ops[5].kind == OpKind::ReadField);
    assert(ops[6].kind == OpKind::Close);
    assert(compiled.operands.rows.size() == 2 &&
           compiled.operands.writes.size() == 1);
    const auto ir = emit_native_i64_pair_ir(compiled.mir, compiled.operands);
    assert(ir.find("define i32 @main()") != std::string::npos);
    assert(ir.find("icmp ult i64") != std::string::npos);
    assert(ir.find("store volatile i1 false, ptr %alive") != std::string::npos);

    {
        auto forged = make_source();
        forged.inverted_main->operations[2].field = "id";
        reject([&]{ (void)verify_production_inverted_i64(forged); },
               "immutable inverted field");
    }
    {
        auto forged = make_source();
        forged.inverted_main->operations[0].index = 1;
        reject([&]{ (void)verify_production_inverted_i64(forged); },
               "invalid inverted push source metadata");
    }
    {
        auto forged = make_source();
        forged.inverted_main->operations[2].values[0] = 91;
        reject([&]{ (void)verify_production_inverted_i64(forged); },
               "invalid inverted write source metadata");
    }
    {
        auto forged = make_source();
        forged.inverted_main->operations[3].field = "x";
        reject([&]{ (void)verify_production_inverted_i64(forged); },
               "invalid inverted erase source metadata");
    }
    {
        auto forged = make_source();
        forged.inverted_main->operations[4].value = 7;
        reject([&]{ (void)verify_production_inverted_i64(forged); },
               "invalid inverted return source metadata");
    }
    {
        auto forged = make_source();
        forged.inverted_main->operations[3].index = 2;
        reject([&]{ (void)verify_production_inverted_i64(forged); },
               "outside current length");
    }
    {
        auto forged = make_source();
        forged.inverted_main->operations.pop_back();
        reject([&]{ (void)verify_production_inverted_i64(forged); },
               "requires terminal field return");
    }
    {
        auto forged = make_source();
        forged.inverted_main->operations.push_back(
            InvertedSourceOperation{InvertedSourceOperation::Kind::Push, 0, "", {{1, 2}}, 0});
        reject([&]{ (void)verify_production_inverted_i64(forged); },
               "terminal read must be last");
    }
    {
        auto forged = make_source();
        forged.inverted_structs[0].fields[1].type = InvertedScalarKind::Bool;
        reject([&]{ (void)verify_production_inverted_i64(forged); },
               "matching two-i64 schema");
    }
    {
        auto forged = make_source();
        forged.inverted_main->capacity = 257;
        reject([&]{ (void)verify_production_inverted_i64(forged); },
               "capacity/operation budget");
    }
    {
        auto forged = make_source();
        forged.inverted_main->schema_name = "Unrelated";
        reject([&]{ (void)verify_production_inverted_i64(forged); },
               "matching two-i64 schema");
    }
    return 0;
}
