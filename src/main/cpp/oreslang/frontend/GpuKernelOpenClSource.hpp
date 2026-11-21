#pragma once

#include "oreslang/frontend/GpuKernelBodyHir.hpp"

#include <string>
#include <vector>

namespace oreslang::gpu::opencl_source {

// This is OpenCL C *source text* for offline syntax and SPIR LLVM IR checks.
// It is NOT a device binary, authenticated queue, memory loan, or permission
// to dispatch. A GPU provider and a separate source-to-runtime proof remain
// mandatory; this object deliberately cannot mint any of those capabilities.
struct Preview {
    std::string source;
    bool signed_extent;
    [[nodiscard]] constexpr bool compiler_device_binary() const noexcept { return false; }
    [[nodiscard]] constexpr bool authenticated_device() const noexcept { return false; }
    [[nodiscard]] constexpr bool executable() const noexcept { return false; }
};

inline Preview emit_saxpy(const std::vector<body_hir::Parameter>& params,
                          const std::vector<body_hir::Token>& tokens) {
    // Never accept a caller-created or modified CheckedBody as trusted proof.
    // Re-run the source checker against actual token kinds and parameter types.
    const auto body = body_hir::check(params, tokens);
    if (params.size() != 5 || body.guard == body_hir::Guard::NoOp ||
            !body.source_body_typed() || body.executable())
        body_hir::reject("GPU-LAUNCH-008", "only checked non-empty SAXPY body emits source");

    const bool signed_extent = params[4].type == body_hir::Type::I64;
    // Fixed names and fixed expression prevent injection from guest-supplied
    // identifier spelling. Do not use `restrict`: host buffer aliasing and
    // provider-issued exclusive loans have NOT yet been authenticated.
    const std::string prefix = R"CL(// AttoLang provisional OpenCL C 1.2 source preview; NOT a GPU binary.
#pragma OPENCL FP_CONTRACT OFF
__kernel void atto_saxpy_v0(
    float scale,
    __global const float* input_x,
    __global const float* input_y,
    __global float* output,
)CL";
    const std::string extent = signed_extent ? "    long count\n" : "    ulong count\n";
    const std::string guard = signed_extent
        ? "    if (count <= 0 || lane >= (ulong)count) return;\n"
        : "    if (lane >= count) return;\n";
    const std::string suffix = R"CL() {
    ulong lane = (ulong)get_global_id(0);
)CL";
    return {prefix + extent + suffix + guard +
        "    output[lane] = scale * input_x[lane] + input_y[lane];\n}\n",
        signed_extent};
}

}  // namespace oreslang::gpu::opencl_source
