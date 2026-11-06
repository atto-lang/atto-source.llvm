// Production Program AST -> typed HIR/MIR -> real i1/i64 LLVM IR.
// This C++17 source-local test does not prove a compiled .atto guest ran.
#include "oreslang/types/InvertedProductionSourceMir.hpp"
#include "oreslang/types/InvertedMirLlvmBoolI64.hpp"
#include <cstdint>
#include <stdexcept>
#include <string>
#include <iostream>

using namespace oreslang;
using namespace oreslang::dop::mir;
static void require(bool condition, const char* label) {
    if (!condition) throw std::runtime_error(label);
}
template<class F> static void reject(F action, const char* diagnostic) {
    try { action(); }
    catch (const std::exception& exc) {
        if (std::string(exc.what()).find(diagnostic) != std::string::npos)
            return;
        throw std::runtime_error(std::string("incorrect rejection: ") + exc.what());
    }
    throw std::runtime_error(std::string("forged mixed source admitted: ") + diagnostic);
}
static Program source() {
    Program p;
    InvertedStructDecl shape;
    shape.name = "Counter";
    shape.fields = {
        InvertedStructField{"active", InvertedScalarKind::Bool, true, 0},
        InvertedStructField{"count", InvertedScalarKind::I64, true, 0}
    };
    p.inverted_structs.push_back(shape);
    InvertedSourceMain main;
    main.schema_name = "Counter";
    main.owner_name = "rows";
    main.capacity = 3;
    using Kind = InvertedSourceOperation::Kind;
    main.operations = {
        InvertedSourceOperation{Kind::Push,0,"",{{1,11}},0},
        InvertedSourceOperation{Kind::Push,0,"",{{0,42}},0},
        InvertedSourceOperation{Kind::WriteField,0,"active",{},1},
        InvertedSourceOperation{Kind::EraseSwap,0,"",{},0},
        InvertedSourceOperation{Kind::ReturnRead,0,"count",{},0}
    };
    p.inverted_main = main;
    return p;
}
int main() {
    const auto valid = source();
    const auto verified = verify_production_inverted_bool_i64(valid);
    const auto& instructions = verified.mir.instructions();
    require(instructions.size()==7,"lost typed MIR operations");
    require(instructions[3].column==0 &&
            instructions[3].kind==OpKind::WriteField,"bool lane not typed");
    require(verified.mir.layout().capacity_bytes(3)==32,
            "mixed HIR physical capacity should include i64 alignment");
    const auto ir = emit_native_bool_i64_pair_ir(verified.mir,verified.operands);
    require(ir.find("%col0 = alloca [3 x i1]")!=std::string::npos,
            "LLVM boolean column was widened to i64");
    require(ir.find("%col1 = alloca [3 x i64]")!=std::string::npos,
            "LLVM i64 column was not typed");
    require(ir.find("store i1 1, ptr")!=std::string::npos,
            "canonical boolean insert/write missing");
    require(ir.find("load i1, ptr")!=std::string::npos,
            "boolean swap-delete must load real i1 cells");
    require(ir.find("store volatile i1 0, ptr")!=std::string::npos,
            "boolean column close must volatile-scrub");
    require(ir.find("icmp ult i64")!=std::string::npos &&
            ir.find("call void @llvm.trap()")!=std::string::npos,
            "runtime owner/bounds trap omitted");
    reject([&]{ (void)verify_production_inverted_i64(valid); },
           "matching two-i64 schema");
    {
        auto bad = source();
        bad.inverted_main->operations[0].values[0]=2;
        reject([&]{ (void)verify_production_inverted_bool_i64(bad); },
               "noncanonical inverted bool push");
    }
    {
        auto bad = source();
        bad.inverted_main->operations[2].value=-1;
        reject([&]{ (void)verify_production_inverted_bool_i64(bad); },
               "noncanonical inverted bool write");
    }
    {
        auto bad = source();
        bad.inverted_main->operations[4].field="active";
        reject([&]{ (void)verify_production_inverted_bool_i64(bad); },
               "cannot read bool");
    }
    {
        auto bad = source();
        bad.inverted_structs[0].fields[0].mutable_field=false;
        reject([&]{ (void)verify_production_inverted_bool_i64(bad); },
               "immutable inverted field");
    }
    {
        auto bad = source();
        bad.inverted_main->operations[4].index=3;
        const auto allowed = verify_production_inverted_bool_i64(bad);
        // The source index is intentionally runtime-checked, not trusted
        // merely because source capacity was compile-time known.
        const auto guarded = emit_native_bool_i64_pair_ir(allowed.mir,allowed.operands);
        require(guarded.find("icmp ult i64 3, ")!=std::string::npos,
                "runtime out-of-bounds index lacks guard");
    }
    {
        BoolI64LlvmEmitter emitter(verified.mir.capacity());
        auto insufficient = verified.operands;
        insufficient.rows.pop_back();
        reject([&]{ (void)emitter.build(verified.mir,insufficient); },
               "missing push operands");
        require(emitter.build(verified.mir,verified.operands)==ir,
                "failed emission poisoned next checked lowering");
        auto forged = verified.operands;
        forged.writes[0]=7;
        reject([&]{ (void)emitter.build(verified.mir,forged); },
               "forged boolean write");
    }
    std::cout << "INVERTED_BOOL_I64_MIR_HOST_PASS groups=7\n";
}
