// Compiler-owned AST -> checked typed HIR/MIR -> emitted LLVM IR.
//
// This deliberately does not parse .atto text. The production guest parser
// must remain fail-closed until its ownership and method-lowering gates land.
// The executable checks the narrow static IR/AOT emission path only.
#include "oreslang/types/InvertedProductionSourceMir.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

int main() {
    using namespace oreslang;
    Program program;
    InvertedStructDecl decl;
    decl.name = "Position";
    decl.fields = {
        InvertedStructField{"x", InvertedScalarKind::I64, true, 0},
        InvertedStructField{"id", InvertedScalarKind::I64, false, 0}
    };
    program.inverted_structs.push_back(decl);

    InvertedSourceMain source;
    source.schema_name = "Position";
    source.owner_name = "rows";
    source.capacity = 3;
    using Kind = InvertedSourceOperation::Kind;
    source.operations = {
        InvertedSourceOperation{Kind::Push, 0, "", {{11, 100}}, 0},
        InvertedSourceOperation{Kind::Push, 0, "", {{20, 101}}, 0},
        InvertedSourceOperation{Kind::WriteField, 1, "x", {}, 42},
        InvertedSourceOperation{Kind::EraseSwap, 0, "", {}, 0},
        InvertedSourceOperation{Kind::ReturnRead, 0, "x", {}, 0}
    };
    program.inverted_main = source;

    const auto verified = dop::mir::verify_production_inverted_i64(program);
    const auto ir = dop::mir::emit_native_i64_pair_ir(verified.mir, verified.operands);
    if (ir.find("define i32 @main()") == std::string::npos ||
        ir.find("llvm.trap") == std::string::npos ||
        ir.find("GC_malloc") != std::string::npos)
        throw std::runtime_error("unchecked or unexpectedly managed inverted MIR IR");
    std::cout << ir;
}
