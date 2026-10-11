#include "oreslang/frontend/AcceleratorSourceLowering.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

static void check(bool ok) {
    if (!ok) throw std::runtime_error("accelerator source lowering regression");
}
template<class F> static void rejects(F fn) {
    try { fn(); }
    catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("invalid accelerator source incorrectly accepted");
}

int main() {
    namespace a = oreslang::accel_source;
    namespace d = oreslang::dop;
    for (auto kind : {d::Layout::AoS, d::Layout::SoA, d::Layout::AoSoA}) {
        const auto lanes = kind == d::Layout::AoSoA ? 4u : 1u;
        d::Plan layout(kind, 7, 3, 4, 16, lanes);
        const std::string source = R"(
            gpu fnc transform(f32 value): f32 {
                return value * 2.0 + 1.0;
            }
        )";
        auto program = a::compile_gpu(source, layout);
        check(program.name == "transform");
        check(program.kernel.operation == d::Op::ScaleBias);
        check(program.kernel.scale == 2 && program.kernel.bias == 1);
        d::HostBuffer input(layout), output(layout);
        input.write(5, 2, 3);
        d::execute_cpu(program.kernel, input, output);
        check(output.read(5, 2) == 7);
        auto emitted = program.emit_opencl({64, 4096, 64, true});
        check(emitted.device_source.find("__kernel void oreslang_dop_f32") != std::string::npos);
        rejects([&] { (void)program.emit_opencl({64, 1, 64, true}); });
    }
    const d::Plan layout(d::Layout::AoSoA, 7, 3, 4, 16, 4);
    for (const auto& body : {
            "return value * 2.0; stdio.println(1);",
            "return value * 2.0 + 1.0 * 3.0;",
            "return unsafe_host(value);",
            "return await value;",
            "return value / 2.0;",
            "return value * 3e400;",
            "return value * 2.0; spawn Worker();"
    }) {
        rejects([&] {
            (void)a::compile_gpu("gpu fnc k(f32 value): f32 { " + std::string(body) + " }", layout);
        });
    }
    auto negative = a::compile_gpu(
        "gpu fnc scale(f32 value): f32 { return value * -2.0; }", layout);
    check(negative.kernel.scale == -2);
    auto minus = a::compile_gpu(
        "gpu fnc bias(f32 value): f32 { return value - 3.0; }", layout);
    check(minus.kernel.bias == -3);

    auto bell = a::compile_quantum(R"(
      quantum fnc bell(): Circuit<2,2> {
        h(0);
        cx(0, 1);
        measure(0,0);
        measure(1,1);
      }
    )");
    check(bell.name == "bell");
    auto probs = oreslang::quantum::simulate_probabilities(bell.circuit);
    check(std::abs(probs[0] - .5) < 1e-12 && probs[1] == 0
        && probs[2] == 0 && std::abs(probs[3] - .5) < 1e-12);
    check(bell.emit_openqasm3().find("cx q[0], q[1];") != std::string::npos);
    for (const auto& body : {
        "h(0); spawn Worker(); measure(0,0);",
        "x(0); await foo(); measure(0,0);",
        "measure(0,0); h(1);",
        "cx(0,0); measure(0,0);",
        "measure(0,0); measure(0,1);",
        "print(0); measure(0,0);",
        "h(0);",
        "measure(3,0);",
        "measure(0,0); abc();"
    }) {
        rejects([&] {
            (void)a::compile_quantum(
                "quantum fnc f(): Circuit<2,2> { " + std::string(body) + " }");
        });
    }
    std::cout << "native accelerator source to typed GPU/quantum IR tests passed\n";
}
