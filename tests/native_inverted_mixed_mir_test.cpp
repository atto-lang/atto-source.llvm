// Strict C++17 independent typed MIR -> LLVM IR admission regression.
// Real source/parser and executable guest are checked by separate tests.
#include "oreslang/types/InvertedMixedGuestLlvm.hpp"
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
using oreslang::InvertedScalarKind;
using oreslang::InvertedSourceLiteral;
using oreslang::InvertedSourceOperation;
using oreslang::InvertedStructDecl;
using oreslang::InvertedStructField;
using oreslang::Program;
using oreslang::InvertedSourceMain;

void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
template<class F>
void rejects(F&& work, const char* expected) {
    try { work(); }
    catch (const std::exception& e) {
        if (std::string(e.what()).find(expected) != std::string::npos) return;
        throw std::runtime_error(std::string("wrong mixed MIR diagnostic: ") + e.what());
    }
    throw std::runtime_error(std::string("mixed MIR admitted: ") + expected);
}
InvertedSourceLiteral i64(std::int64_t value) {
    InvertedSourceLiteral x;
    x.kind = InvertedScalarKind::I64; x.integer = value;
    return x;
}
InvertedSourceLiteral bit(bool value) {
    InvertedSourceLiteral x;
    x.kind = InvertedScalarKind::Bool; x.boolean = value;
    return x;
}
InvertedSourceLiteral f64(double value) {
    InvertedSourceLiteral x;
    x.kind = InvertedScalarKind::F64; x.floating = value;
    return x;
}
Program make_source() {
    Program source;
    InvertedStructDecl decl;
    decl.name = "Particle";
    decl.fields = {
        InvertedStructField{"x", InvertedScalarKind::I64, true, 0},
        InvertedStructField{"alive", InvertedScalarKind::Bool, false, 0},
        InvertedStructField{"energy", InvertedScalarKind::F64, true, 0}
    };
    source.inverted_structs.push_back(std::move(decl));
    InvertedSourceMain main;
    main.schema_name = "Particle"; main.owner_name = "rows";
    main.capacity = 3; main.three_scalar = true;
    using Kind = InvertedSourceOperation::Kind;
    InvertedSourceOperation first;
    first.kind = Kind::Push;
    first.mixed_values = {i64(11),bit(true),f64(1.5)};
    main.operations.push_back(first);
    InvertedSourceOperation second;
    second.kind = Kind::Push;
    second.mixed_values = {i64(42),bit(false),f64(-2.25)};
    main.operations.push_back(second);
    InvertedSourceOperation write;
    write.kind = Kind::WriteField; write.index = 1;
    write.field = "energy"; write.mixed_value = f64(3.75);
    main.operations.push_back(write);
    InvertedSourceOperation erase;
    erase.kind = Kind::EraseSwap; erase.index = 0;
    main.operations.push_back(erase);
    InvertedSourceOperation read;
    read.kind = Kind::ReturnRead; read.index = 0; read.field = "x";
    main.operations.push_back(read);
    source.inverted_main = std::move(main);
    return source;
}
} // namespace

int main() {
    using namespace oreslang::dop::mir;
    const auto valid = make_source();
    const auto checked = verify_production_inverted_mixed(valid);
    const auto& code = checked.mir.instructions();
    require(code.size() == 7, "mixed MIR should allocate+push+push+write+erase+read+close");
    require(code.front().kind == OpKind::Allocate, "mixed MIR missing allocation");
    require(code.back().kind == OpKind::Close, "mixed MIR did not revoke owner");
    const auto ir = emit_mixed_source_ir(checked);
    require(ir.find("define i32 @main()") != std::string::npos, "no guest main");
    require(ir.find("alloca [3 x i64]") != std::string::npos, "missing i64 column");
    require(ir.find("alloca [3 x i1]") != std::string::npos, "missing bool column");
    require(ir.find("alloca [3 x double]") != std::string::npos, "missing f64 column");
    require(ir.find("0x400E000000000000") != std::string::npos, "3.75 bitcast lost");
    require(ir.find("store volatile double") != std::string::npos, "f64 teardown omitted");
    require(ir.find("call void @llvm.trap()") != std::string::npos, "bounds trap omitted");
    require(ir.find("store volatile i1 false, ptr %alive") != std::string::npos,
            "owner revocation omitted");

    {
        auto bad=make_source(); bad.inverted_main->three_scalar=false;
        rejects([&]{ (void)verify_production_inverted_mixed(bad); },
                "matching three-scalar schema");
    }
    {
        auto bad=make_source(); bad.inverted_main->operations[0].mixed_values[1]=i64(1);
        rejects([&]{ (void)verify_production_inverted_mixed(bad); },
                "scalar type mismatch");
    }
    {
        auto bad=make_source();
        bad.inverted_main->operations[0].mixed_values[0].boolean=true;
        rejects([&]{ (void)verify_production_inverted_mixed(bad); },
                "hidden variant payload");
    }
    {
        auto bad=make_source();
        bad.inverted_main->operations[0].mixed_values[1].floating=2.5;
        rejects([&]{ (void)verify_production_inverted_mixed(bad); },
                "hidden variant payload");
    }
    {
        auto bad=make_source();
        bad.inverted_main->operations[0].mixed_values[1].floating=-0.0;
        rejects([&]{ (void)verify_production_inverted_mixed(bad); },
                "hidden variant payload");
    }
    {
        auto bad=make_source();
        bad.inverted_main->operations[3].mixed_value.floating=-0.0;
        rejects([&]{ (void)verify_production_inverted_mixed(bad); },
                "invalid mixed inverted erase metadata");
    }
    {
        auto valid_negative_zero=make_source();
        valid_negative_zero.inverted_main->operations[0].mixed_values[2]=f64(-0.0);
        const auto admitted=verify_production_inverted_mixed(valid_negative_zero);
        const auto ir_negative_zero=emit_mixed_source_ir(admitted);
        require(ir_negative_zero.find("0x8000000000000000") != std::string::npos,
                "active mixed f64 negative zero lost its IEEE sign bit");
    }
    {
        auto bad=make_source(); bad.inverted_main->operations[2].field="alive";
        bad.inverted_main->operations[2].mixed_value=bit(false);
        rejects([&]{ (void)verify_production_inverted_mixed(bad); },
                "immutable inverted field");
    }
    {
        auto bad=make_source(); bad.inverted_main->operations[2].mixed_value=f64(
                std::numeric_limits<double>::infinity());
        rejects([&]{ (void)verify_production_inverted_mixed(bad); }, "must be finite");
    }
    {
        auto bad=make_source(); bad.inverted_main->operations.back().field="energy";
        rejects([&]{ (void)verify_production_inverted_mixed(bad); }, "must return i64");
    }
    {
        auto bad=make_source(); bad.inverted_main->operations[3].index=2;
        rejects([&]{ (void)verify_production_inverted_mixed(bad); },
                "outside current length");
    }
    {
        auto bad=make_source(); bad.inverted_main->operations[1].value=1;
        rejects([&]{ (void)verify_production_inverted_mixed(bad); },
                "legacy scalar metadata");
    }
    {
        auto bad=make_source();
        bad.inverted_main->operations[3].mixed_values[0]=bit(true);
        rejects([&]{ (void)verify_production_inverted_mixed(bad); },
                "invalid mixed inverted erase metadata");
    }
    {
        auto bad=make_source();
        bad.inverted_main->operations[4].mixed_value=f64(6.5);
        rejects([&]{ (void)verify_production_inverted_mixed(bad); },
                "invalid mixed inverted return metadata");
    }
    {
        auto bad=make_source(); bad.inverted_main->capacity=257;
        rejects([&]{ (void)verify_production_inverted_mixed(bad); },
                "capacity/operation budget");
    }
    {
        MixedLlvmEmitter emitter(3);
        require(emitter.build(checked.mir, checked.operands)==ir, "first emission mismatch");
        auto incomplete=checked.operands;
        incomplete.rows.pop_back();
        rejects([&]{ (void)emitter.build(checked.mir,incomplete); },
                "missing push operands");
        require(emitter.build(checked.mir,checked.operands)==ir,
                "emitter leaked state after failed build");
        auto changed=checked.operands;
        changed.writes[0]=f64(5.5);
        require(emitter.build(checked.mir,changed)!=ir,
                "emitter discarded changed f64 write operand");
    }
    return 0;
}
