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

int main(int argc, char** argv) {
    const std::string mode = argc == 2 ? argv[1] : "";
    const bool negative = mode == "--bounds-trap";
    if (argc > 2 || !(mode.empty() || negative || mode == "--bulk-reuse" ||
                      mode == "--tail-delete" || mode == "--overwrite"))
        throw std::invalid_argument("unexpected MIR smoke argument");
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
    if (negative) {
        // Deleting dense row 0 from two rows moves row 1 to position 0.
        // The old dense position 1 must TRAP at runtime, not return 42 or
        // be resurrected as a generation-checked object handle.
        source.operations = {
            InvertedSourceOperation{Kind::Push, 0, "", {{11, 100}}, 0},
            InvertedSourceOperation{Kind::Push, 0, "", {{20, 101}}, 0},
            InvertedSourceOperation{Kind::EraseSwap, 0, "", {}, 0},
            InvertedSourceOperation{Kind::ReturnRead, 1, "x", {}, 0}
        };
    }
    if (mode == "--bulk-reuse") {
        // Drive the physical native columns through many insertions and
        // swap-erases, then reuse freed capacity. The final row is exactly 42.
        source.capacity = 64;
        source.operations.clear();
        for (std::int64_t i = 0; i < 64; ++i)
            source.operations.push_back(
                InvertedSourceOperation{Kind::Push, 0, "", {{i, i + 1000}}, 0});
        for (std::size_t i = 0; i < 32; ++i)
            source.operations.push_back(
                InvertedSourceOperation{Kind::EraseSwap, 0, "", {}, 0});
        source.operations.push_back(
            InvertedSourceOperation{Kind::Push, 0, "", {{42, 4242}}, 0});
        source.operations.push_back(
            InvertedSourceOperation{Kind::ReturnRead, 32, "x", {}, 0});
    } else if (mode == "--tail-delete") {
        source.operations = {
            InvertedSourceOperation{Kind::Push, 0, "", {{42, 111}}, 0},
            InvertedSourceOperation{Kind::Push, 0, "", {{25, 222}}, 0},
            InvertedSourceOperation{Kind::EraseSwap, 1, "", {}, 0},
            InvertedSourceOperation{Kind::ReturnRead, 0, "x", {}, 0}
        };
    } else if (mode == "--overwrite") {
        source.operations = {
            InvertedSourceOperation{Kind::Push, 0, "", {{0, 88}}, 0},
            InvertedSourceOperation{Kind::WriteField, 0, "x", {}, 42},
            InvertedSourceOperation{Kind::ReturnRead, 0, "x", {}, 0}
        };
    }
    program.inverted_main = source;

    const auto verified = dop::mir::verify_production_inverted_i64(program);
    const auto ir = dop::mir::emit_native_i64_pair_ir(verified.mir, verified.operands);
    if (ir.find("define i32 @main()") == std::string::npos ||
        ir.find("llvm.trap") == std::string::npos ||
        ir.find("GC_malloc") != std::string::npos)
        throw std::runtime_error("unchecked or unexpectedly managed inverted MIR IR");
    std::cout << ir;
}
