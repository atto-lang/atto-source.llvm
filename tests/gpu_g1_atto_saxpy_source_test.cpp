#include "oreslang/backend/GpuG1AttoSaxpySource.hpp"
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
int assertions = 0;
void check(bool pass) { ++assertions; if (!pass) throw std::runtime_error("G1 source test failed"); }
template <class Fn> void rejects(Fn&& fn) {
    bool rejected = false;
    try { fn(); } catch (const std::invalid_argument&) { rejected = true; }
    check(rejected);
}
}
int main() {
    using oreslang::gpu::g1::compile_g1_saxpy_source;
    const std::string kernel = R"ATTO(pub gpu fnc saxpy(f32 scale, GpuArray<f32> x, GpuArray<f32> y,
        GpuArray<f32> mut out, u64 n) => void {
            const u64 i = gpu.global_id(0);
            if i >= n { return; }
            out[i] = scale * x[i] + y[i];
        })ATTO";
    const auto compiled = compile_g1_saxpy_source(kernel);
    check(compiled.tokens > 60);
    check(compiled.llvm_ir.find("define ptx_kernel void @atto_g1_saxpy_f32") != std::string::npos);
    check(compiled.llvm_ir.find("icmp ult i64 %index, %n") != std::string::npos);
    check(compiled.llvm_ir.find("mul nuw") == std::string::npos);
    check(compile_g1_saxpy_source("// verified\n" + kernel).llvm_ir == compiled.llvm_ir);
    const auto final = kernel.rfind('}');
    check(compile_g1_saxpy_source(kernel.substr(0, final) + "return; }").llvm_ir == compiled.llvm_ir);
    const std::vector<std::pair<std::string,std::string>> changes = {
        {"gpu fnc", "fnc"}, {"saxpy(", "hack("}, {"f32 scale", "f64 scale"},
        {"GpuArray<f32> x", "HostBuffer<f32> x"}, {"GpuArray<f32> mut out", "GpuArray<f32> out"},
        {"GpuArray<f32> y", "GpuArray<f32> mut y"}, {"u64 n", "i64 n"},
        {"const u64 i", "let u64 i"}, {"gpu.global_id(0)", "gpu.global_id(1)"},
        {"if i >= n", "if i > n"}, {"if i >= n { return; }", ""},
        {"out[i] =", "x[i] ="}, {"scale * x[i]", "scale * y[i]"},
        {"x[i] + y[i]", "x[i] - y[i]"}, {"out[i] =", "out[i + 1] ="},
        {"return;", "await sleep(1);"}, {"out[i] = scale", "gpu.group_barrier(); out[i] = scale"},
        {"const u64 i", "const u64 j"}, {"scale * x[i]", "x[i] * scale"}
    };
    for (const auto& change : changes) {
        auto mutated = kernel;
        auto i = mutated.find(change.first);
        check(i != std::string::npos);
        mutated.replace(i, change.first.size(), change.second);
        rejects([&]{ (void)compile_g1_saxpy_source(mutated); });
    }
    rejects([&]{ (void)compile_g1_saxpy_source(kernel + " pub fnc evil() => void {}"); });
    rejects([&]{ (void)compile_g1_saxpy_source(kernel + " /* unclosed"); });
    rejects([&]{ (void)compile_g1_saxpy_source(kernel + "\x01"); });
    rejects([&]{ (void)compile_g1_saxpy_source(""); });
    rejects([&]{ (void)compile_g1_saxpy_source(std::string(65537, 'x')); });
    rejects([&]{ (void)compile_g1_saxpy_source(kernel + " // trailing\n ;"); });
    std::cout << "PASS " << assertions << " G1 restricted source compiler assertions (HOST ONLY)\n";
}
