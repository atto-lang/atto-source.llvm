#include "oreslang/frontend/Parser.hpp"
#include "oreslang/runtime/DopKernelIR.hpp"
#include <iostream>
#include <stdexcept>
#include <string>

template<class F> static void rejects(F f) {
    try { f(); } catch (const std::exception&) { return; }
    throw std::runtime_error("unsafe accelerator source unexpectedly admitted");
}

int main() {
    oreslang::dop::Plan layout(oreslang::dop::Layout::AoSoA, 7, 3, 4, 16, 4);
    auto result = oreslang::compile_gpu_source(
        "gpu fnc transform(f32 value): f32 { return value * 2.0 + 1.0; }",
        layout, {64, 4096, 64, true});
    if (result.required_bytes != layout.byte_length()
        || result.device_source.find("__kernel void oreslang_dop_f32") == std::string::npos)
        throw std::runtime_error("native compiler failed to emit typed GPU IR");

    const auto qasm = oreslang::compile_quantum_source_openqasm3(
        "quantum fnc bell(): Circuit<2,2> { h(0); cx(0,1); measure(0,0); measure(1,1); }");
    if (qasm.find("cx q[0], q[1];") == std::string::npos)
        throw std::runtime_error("native compiler failed to emit typed QASM3");

    // General LLVM guest compiler must still reject GPU/QPU execution.
    rejects([] { (void)oreslang::parse_program(
        "gpu fnc k(f32 value): f32 { return value * 2.0; }"); });
    rejects([] { (void)oreslang::parse_program(
        "quantum fnc q(): Circuit<1,1> { measure(0,0); }"); });
    rejects([&] { (void)oreslang::compile_gpu_source(
        "gpu fnc unsafe(f32 value): f32 { return await value; }",
        layout, {64, 4096, 64, true}); });
    rejects([&] { (void)oreslang::compile_gpu_source(
        "gpu fnc k(f32 value): f32 { return value * 2.0; }",
        layout, {64, 1, 64, true}); });
    rejects([] { (void)oreslang::compile_quantum_source_openqasm3(
        "quantum fnc unsafe(): Circuit<1,1> { print(0); measure(0,0); }"); });
    std::cout << "native main Parser compiler boundaries and guest rejection PASS\n";
}
