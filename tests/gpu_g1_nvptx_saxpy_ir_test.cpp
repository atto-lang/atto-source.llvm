#include "oreslang/backend/GpuG1NvptxSaxpyIr.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
int checks = 0;
void check(bool condition) {
    ++checks;
    if (!condition) std::abort();
}
template <class Fn> void rejects(Fn&& f) {
    bool rejected = false;
    try { f(); } catch (const std::invalid_argument& error) {
        rejected = std::string_view(error.what()).find("GPU-G1-IR-001") != std::string_view::npos;
    }
    check(rejected);
}
} // namespace

int main(int argc, char** argv) {
    using namespace oreslang::gpu::g1;
    const SaxpyContract canonical{};
    const auto ir = lower_saxpy_to_nvptx_llvm_ir(canonical);
    // Emit LLVM IR without a shell, filesystem side effect, PTX claim or GPU
    // access. The host runner (if present) must independently verify it.
    if (argc == 2 && std::string_view(argv[1]) == "--emit-llvm-ir") {
        std::cout << ir;
        return 0;
    }
    if (argc != 1) return 2;
    check(ir.find("target triple = \"nvptx64-nvidia-cuda\"") != std::string::npos);
    check(ir.find("define ptx_kernel void @atto_g1_saxpy_f32(") != std::string::npos);
    check(ir.find("ptr addrspace(1) %out") != std::string::npos);
    check(ir.find("llvm.nvvm.read.ptx.sreg.ctaid.x()") != std::string::npos);
    check(ir.find("llvm.nvvm.read.ptx.sreg.ntid.x()") != std::string::npos);
    check(ir.find("llvm.nvvm.read.ptx.sreg.tid.x()") != std::string::npos);
    check(ir.find("icmp ult i64 %index, %n") != std::string::npos);
    check(ir.find("br i1 %in_bounds, label %body, label %exit") != std::string::npos);
    check(ir.find("load float, ptr addrspace(1)") != std::string::npos);
    check(ir.find("store float %sum, ptr addrspace(1) %op") != std::string::npos);
    check(ir.find("fmul float %scale, %a") != std::string::npos);
    check(ir.find("fadd float %scaled, %b") != std::string::npos);
    check(ir.find("getelementptr inbounds") == std::string::npos);
    check(ir.find(" fast ") == std::string::npos);
    check(ir.find("gpu.group_barrier") == std::string::npos);
    check(ir == lower_saxpy_to_nvptx_llvm_ir(canonical));
    SaxpyContract bad;
    bad.abi_version = 2; rejects([&] { (void)lower_saxpy_to_nvptx_llvm_ir(bad); });
    bad = canonical; bad.element = ScalarElement::F64;
    rejects([&] { (void)lower_saxpy_to_nvptx_llvm_ir(bad); });
    bad = canonical; bad.buffers = MemorySpace::Host;
    rejects([&] { (void)lower_saxpy_to_nvptx_llvm_ir(bad); });
    bad = canonical; bad.input = Access::ExclusiveWrite;
    rejects([&] { (void)lower_saxpy_to_nvptx_llvm_ir(bad); });
    bad = canonical; bad.output = Access::ReadOnly;
    rejects([&] { (void)lower_saxpy_to_nvptx_llvm_ir(bad); });
    bad = canonical; bad.indexed_by_global_id_x = false;
    rejects([&] { (void)lower_saxpy_to_nvptx_llvm_ir(bad); });
    bad = canonical; bad.bounds_checked_against_n = false;
    rejects([&] { (void)lower_saxpy_to_nvptx_llvm_ir(bad); });
    bad = canonical; bad.permits_device_await = true;
    rejects([&] { (void)lower_saxpy_to_nvptx_llvm_ir(bad); });
    bad = canonical; bad.permits_host_captures = true;
    rejects([&] { (void)lower_saxpy_to_nvptx_llvm_ir(bad); });
    bad = canonical; bad.permits_implicit_host_transfer = true;
    rejects([&] { (void)lower_saxpy_to_nvptx_llvm_ir(bad); });
    bad = canonical; bad.permits_implicit_cpu_fallback = true;
    rejects([&] { (void)lower_saxpy_to_nvptx_llvm_ir(bad); });
    std::cout << "PASS " << checks << " host-only typed LLVM NVPTX SAXPY IR assertions\n";
}
