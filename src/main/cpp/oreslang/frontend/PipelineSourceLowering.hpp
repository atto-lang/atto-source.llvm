#pragma once

#include "oreslang/frontend/AcceleratorSourceLowering.hpp"
#include "oreslang/runtime/DopKernelIR.hpp"

#include <array>
#include <string>
#include <string_view>
#include <stdexcept>
#include <utility>

namespace oreslang::pipeline_source {

/**
 * Minimal executable *host-compiled* actor graph admission. The specified
 * actor roles have fixed, audited host implementations. Source-defined actor
 * methods, free variables, host effects, calls and arbitrary stages are not
 * executable through this profile. Normal LLVM guest parse remains closed.
 *
 * The same bounded accelerator token admission is used for the entire file.
 */
struct Stage {
    std::string name;
    std::string input;
    std::string output;
};
struct Plan {
    std::array<Stage, 3> stages;
    std::string entry;
    std::string gpu_function;
    dop::OpenClArtifact kernel;
};

inline Plan compile(std::string_view source, dop::Plan layout, dop::GpuTarget target) {
    accel_source::Cursor cursor(source);
    constexpr std::array<std::string_view, 3> allowed{
        "CpuWorker", "GpuWorker", "CollectResultsWorker"};
    std::array<Stage, 3> stages;
    std::array<bool, 3> seen{};
    for (std::size_t i = 0; i < 3; ++i) {
        cursor.expect("define");
        cursor.expect("actor");
        const std::string name = cursor.identifier();
        std::size_t slot = allowed.size();
        for (std::size_t j = 0; j < allowed.size(); ++j) {
            if (allowed[j] == name) slot = j;
        }
        if (slot == allowed.size() || seen[slot])
            cursor.fail("unknown or duplicate pipeline actor implementation");
        seen[slot] = true;
        cursor.expect("implements");
        cursor.expect("StreamOperator");
        cursor.expect("<");
        auto input = cursor.identifier();
        cursor.expect(",");
        auto output = cursor.identifier();
        cursor.expect(">");
        cursor.expect("as");
        // An actor with a user-defined body requires full actor effects,
        // ownership and supervisor semantics. Never pretend to execute it.
        cursor.expect("end");
        stages[slot] = {name, std::move(input), std::move(output)};
    }
    if (stages[0].input != "void" || stages[0].output != "f32"
        || stages[1].input != "f32" || stages[1].output != "f32"
        || stages[2].input != "f32" || stages[2].output != "Results")
        cursor.fail("pipeline StreamOperator ports do not match audited f32 graph");

    cursor.expect("gpu");
    cursor.expect("fnc");
    auto gpu_name = cursor.identifier();
    if (gpu_name != "transform") cursor.fail("GPU worker must bind audited transform");
    cursor.expect("(");
    cursor.expect("f32");
    auto param = cursor.identifier();
    cursor.expect(")");
    cursor.expect(":");
    cursor.expect("f32");
    cursor.expect("{");
    cursor.expect("return");
    cursor.expect(param);
    float scale = 1.0f, bias = 0.0f;
    bool mult = cursor.take("*");
    if (mult) scale = cursor.f32();
    const bool plus = cursor.take("+");
    const bool minus = plus ? false : cursor.take("-");
    if (plus || minus) {
        bias = cursor.f32();
        if (minus) bias = -bias;
    }
    cursor.expect(";");
    cursor.expect("}");
    const auto operation = mult && (plus || minus) ? dop::Op::ScaleBias
            : mult ? dop::Op::Scale : dop::Op::Bias;
    dop::Kernel typed(layout, dop::Element::F32, operation, scale, bias);
    auto kernel = dop::lower_opencl(typed, target);

    cursor.expect("pub");
    cursor.expect("fnc");
    auto entry = cursor.identifier();
    cursor.expect("(");
    cursor.expect(")");
    cursor.expect(":");
    cursor.expect("ActorStream");
    cursor.expect("<");
    cursor.expect("void");
    cursor.expect(",");
    cursor.expect("Results");
    cursor.expect(">");
    cursor.expect("{");
    cursor.expect("return");
    for (std::size_t i = 0; i < stages.size(); ++i) {
        if (i > 0) cursor.expect("pipe");
        cursor.expect("spawn");
        cursor.expect(stages[i].name);
        cursor.expect("(");
        cursor.expect(")");
    }
    cursor.expect(";");
    cursor.expect("}");
    cursor.expect("<eof>");

    return Plan{std::move(stages), std::move(entry), std::move(gpu_name),
                std::move(kernel)};
}

} // namespace oreslang::pipeline_source
