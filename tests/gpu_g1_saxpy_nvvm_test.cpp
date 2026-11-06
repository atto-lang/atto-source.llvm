#include "oreslang/gpu/G1SaxpyNvvm.hpp"
#include <stdexcept>
#include <string>
#include <iostream>
#include <vector>

using oreslang::gpu::g1::compile_saxpy_nvvm;
static int checked = 0;
static void assert_true(bool value) {
    if (!value) throw std::runtime_error("GPU-G1 native source compiler test failed");
    ++checked;
}
template<class Fn> static void rejects(Fn fn) {
    try { fn(); } catch (const std::invalid_argument& e) {
        assert_true(std::string(e.what()).find("GPU-G1-001") != std::string::npos);
        return;
    }
    throw std::runtime_error("unsafe mutation was not rejected");
}
static const std::string kernel = R"ATTO(
pub gpu fnc saxpy(f32 scale, GpuArray<f32> x,
                    GpuArray<f32> y, GpuArray<f32> mut out, u64 n) => void {
  const u64 i = gpu.global_id(0);
  if i >= n { return; }
  out[i] = scale * x[i] + y[i];
}
)ATTO";
int main() {
    const auto baseline = compile_saxpy_nvvm(kernel);
    const auto& ir = baseline.llvm_ir;
    for (std::string_view s : {"nvptx64-nvidia-cuda", "ptx_kernel void @saxpy",
            "llvm.nvvm.read.ptx.sreg.ctaid.x", "llvm.nvvm.read.ptx.sreg.ntid.x",
            "llvm.nvvm.read.ptx.sreg.tid.x", "icmp ult i64 %i, %n",
            "getelementptr float, ptr addrspace(1)", "fmul float %scale, %vx",
            "fadd float %scaled, %vy", "store float %sum", "br i1 %inside"}) {
        assert_true(ir.find(s) != std::string::npos);
    }
    assert_true(ir.find("mul nuw i64") != std::string::npos); // 64-bit indexing
    assert_true(baseline.parsed_tokens > 60);
    assert_true(compile_saxpy_nvvm("// comment\n" + kernel).llvm_ir == ir);
    const std::string terminal = kernel.substr(0, kernel.rfind('}')) + " return; }";
    assert_true(compile_saxpy_nvvm(terminal).llvm_ir == ir);
    struct Mutation { const char* from; const char* to; };
    const std::vector<Mutation> mutations = {
        {"gpu fnc", "fnc"}, {"f32 scale", "f64 scale"},
        {"GpuArray<f32> x", "HostBuffer<f32> x"},
        {"GpuArray<f32> mut out", "GpuArray<f32> out"},
        {"gpu.global_id(0)", "gpu.global_id(1)"},
        {"i >= n", "i > n"}, {"if i >= n { return; }", ""},
        {"out[i]", "x[i]"}, {"scale * x[i]", "x[i] * scale"},
        {"x[i] + y[i]", "x[i] - y[i]"},
        {"scale * x[i]", "scale / x[i]"},
        {"out[i] =", "out[i + 1] ="},
        {"const u64 i", "let u64 i"},
        {"return;", "await sleep(1);"},
        {"out[i] = scale * x[i] + y[i];", "out[i] = scale * x[i] + y[i]; gpu.group_barrier();"},
        {"u64 n", "i64 n"},
        {"saxpy(", "saxpy2("},
        {"= scale *", "= 2 * scale *"},
    };
    for (const auto& mutation : mutations) {
        auto modified = kernel;
        const auto position = modified.find(mutation.from);
        assert_true(position != std::string::npos);
        modified.replace(position, std::string(mutation.from).size(), mutation.to);
        rejects([&] { (void)compile_saxpy_nvvm(modified); });
    }
    rejects([] { (void)compile_saxpy_nvvm(""); });
    rejects([] { (void)compile_saxpy_nvvm("/* unterminated"); });
    rejects([] { (void)compile_saxpy_nvvm("pub gpu fnc saxpy() {}\n\x01"); });
    rejects([] { (void)compile_saxpy_nvvm(std::string(65537, 'x')); });
    rejects([] { (void)compile_saxpy_nvvm(kernel + "pub fnc hostile(): void {}"); });
    std::cout << "PASS " << checked << " G1 source-to-NVVM IR admission assertions (HOST ONLY)\n";
}
