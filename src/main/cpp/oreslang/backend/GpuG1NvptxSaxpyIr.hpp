#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace oreslang::gpu::g1 {

// G1 is a deliberately closed typed DEVICE IR proof, NOT an .atto frontend.
// The source compiler must subsequently prove that a gpu fnc produces this
// exact ABI/effect/bounds description before using this lowering.
enum class ScalarElement : std::uint8_t { F32, F64 };
enum class MemorySpace : std::uint8_t { Device, Host };
enum class Access : std::uint8_t { ReadOnly, ExclusiveWrite };

struct SaxpyContract {
    std::uint32_t abi_version = 1;
    ScalarElement element = ScalarElement::F32;
    MemorySpace buffers = MemorySpace::Device;
    Access input = Access::ReadOnly;
    Access output = Access::ExclusiveWrite;
    bool indexed_by_global_id_x = true;
    bool bounds_checked_against_n = true;
    bool permits_device_await = false;
    bool permits_host_captures = false;
    bool permits_implicit_host_transfer = false;
    bool permits_implicit_cpu_fallback = false;
};

inline void validate_saxpy_contract(const SaxpyContract& c) {
    if (c.abi_version != 1 || c.element != ScalarElement::F32 ||
        c.buffers != MemorySpace::Device || c.input != Access::ReadOnly ||
        c.output != Access::ExclusiveWrite || !c.indexed_by_global_id_x ||
        !c.bounds_checked_against_n || c.permits_device_await ||
        c.permits_host_captures || c.permits_implicit_host_transfer ||
        c.permits_implicit_cpu_fallback)
        throw std::invalid_argument(
            "GPU-G1-IR-001: unsupported or unsafe SAXPY device IR; no CPU fallback");
}

inline constexpr std::string_view kEntry = "atto_g1_saxpy_f32";
inline constexpr std::string_view kTargetTriple = "nvptx64-nvidia-cuda";

// LLVM IR, not PTX or a physical GPU result. The only injected values are
// compiler-owned constants; no raw source identifier or user text is spliced.
// GPU global pointers use addrspace(1); the caller must prove at least n
// readable f32 elements in x/y and n exclusively writable elements in out.
// No inbounds GEP assumption, fast-math flags, divergent barrier or host IO.
inline std::string lower_saxpy_to_nvptx_llvm_ir(const SaxpyContract& c) {
    validate_saxpy_contract(c);
    return R"IR(; atto.gpu.g1.saxpy.f32/1 ; typed device IR experiment ONLY
; NOT evidence of parsed .atto source or physical GPU execution.
target triple = "nvptx64-nvidia-cuda"

declare i32 @llvm.nvvm.read.ptx.sreg.ctaid.x()
declare i32 @llvm.nvvm.read.ptx.sreg.ntid.x()
declare i32 @llvm.nvvm.read.ptx.sreg.tid.x()

define ptx_kernel void @atto_g1_saxpy_f32(float %scale, ptr addrspace(1) %x, ptr addrspace(1) %y, ptr addrspace(1) %out, i64 %n) {
entry:
  %bid = call i32 @llvm.nvvm.read.ptx.sreg.ctaid.x()
  %bd = call i32 @llvm.nvvm.read.ptx.sreg.ntid.x()
  %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
  %bid64 = zext i32 %bid to i64
  %bd64 = zext i32 %bd to i64
  %tid64 = zext i32 %tid to i64
  %base = mul i64 %bid64, %bd64
  %index = add i64 %base, %tid64
  %in_bounds = icmp ult i64 %index, %n
  br i1 %in_bounds, label %body, label %exit
body:
  %xp = getelementptr float, ptr addrspace(1) %x, i64 %index
  %yp = getelementptr float, ptr addrspace(1) %y, i64 %index
  %op = getelementptr float, ptr addrspace(1) %out, i64 %index
  %a = load float, ptr addrspace(1) %xp, align 4
  %b = load float, ptr addrspace(1) %yp, align 4
  %scaled = fmul float %scale, %a
  %sum = fadd float %scaled, %b
  store float %sum, ptr addrspace(1) %op, align 4
  br label %exit
exit:
  ret void
}
)IR";
}

} // namespace oreslang::gpu::g1
