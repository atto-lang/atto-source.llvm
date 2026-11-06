#pragma once

#include <cctype>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace oreslang::gpu::g1 {

// A deliberately tiny first source-to-NVVM kernel compiler. The exact
// SAXPY AST shape below is its entire admitted language, NOT general
// AttoLang GPU support or an authorization to submit a GPU command.
// Keep this independent of the normal scalar compiler's fail-closed gate.
struct CompileResult {
    std::string llvm_ir;
    std::size_t parsed_tokens;
};

inline std::vector<std::string> lex(std::string_view source) {
    if (source.empty() || source.size() > 64 * 1024)
        throw std::invalid_argument("GPU-G1-001: source must contain 1..65536 bytes");
    std::vector<std::string> out;
    for (std::size_t i = 0; i < source.size();) {
        const auto ch = static_cast<unsigned char>(source[i]);
        if (std::isspace(ch)) { ++i; continue; }
        if (source.substr(i, 2) == "//") {
            i += 2;
            while (i < source.size() && source[i] != '\n') ++i;
            continue;
        }
        if (source.substr(i, 2) == "/*") {
            const auto close = source.find("*/", i + 2);
            if (close == std::string_view::npos ||
                    source.substr(i + 2, close - (i + 2)).find("/*") != std::string_view::npos)
                throw std::invalid_argument("GPU-G1-001: malformed or nested comment");
            i = close + 2;
            continue;
        }
        const std::size_t start = i;
        if (std::isalpha(ch) || ch == '_') {
            ++i;
            while (i < source.size()) {
                const auto part = static_cast<unsigned char>(source[i]);
                if (!std::isalnum(part) && part != '_') break;
                ++i;
            }
        } else if (std::isdigit(ch)) {
            ++i;
            while (i < source.size() &&
                   std::isdigit(static_cast<unsigned char>(source[i]))) ++i;
        } else if (source.substr(i, 2) == ">=") {
            i += 2;
        } else if (std::string_view("(){};,<>[]=+*.").find(static_cast<char>(ch))
                   != std::string_view::npos) {
            ++i;
        } else {
            throw std::invalid_argument("GPU-G1-001: unsupported device-source token");
        }
        out.emplace_back(source.substr(start, i - start));
        if (out.size() > 512)
            throw std::invalid_argument("GPU-G1-001: device-source token budget exceeded");
    }
    return out;
}

inline CompileResult compile_saxpy_nvvm(std::string_view source) {
    const auto tokens = lex(source);
    // This closed grammar is intentionally explicit: an unknown effect,
    // absent bounds guard, altered pointer/write footprint, host function,
    // second kernel or extra statement is rejected before IR emission.
    static constexpr std::string_view admitted[] = {
        "pub", "gpu", "fnc", "saxpy", "(",
        "f32", "scale", ",", "GpuArray", "<", "f32", ">", "x", ",",
        "GpuArray", "<", "f32", ">", "y", ",",
        "GpuArray", "<", "f32", ">", "mut", "out", ",",
        "u64", "n", ")", "=", ">", "void", "{",
        "const", "u64", "i", "=", "gpu", ".", "global_id", "(", "0", ")", ";",
        "if", "i", ">=", "n", "{", "return", ";", "}",
        "out", "[", "i", "]", "=", "scale", "*", "x", "[", "i", "]",
        "+", "y", "[", "i", "]", ";", "}"
    };
    constexpr std::size_t total = sizeof(admitted) / sizeof(admitted[0]);
    if (tokens.size() != total && !(tokens.size() == total + 2 &&
          tokens[total - 1] == "return" && tokens[total] == ";")) {
        throw std::invalid_argument("GPU-G1-001: unsupported kernel source shape");
    }
    for (std::size_t i = 0; i < total; ++i) {
        // An optional trailing return; above shape must still be identical
        // before we permit a distinct final terminator.
        if (i == total - 1 && tokens.size() == total + 2) {
            if (tokens[i] != "return" || tokens[i + 1] != ";" || tokens[i + 2] != "}")
                throw std::invalid_argument("GPU-G1-001: invalid terminal return");
        } else if (tokens[i] != admitted[i]) {
            throw std::invalid_argument("GPU-G1-001: unproved kernel effect or source grammar");
        }
    }
    static constexpr std::string_view nvvm_ir = R"IR(; AttoLang GPU-G1: verified narrow SAXPY source subset, not physical-device evidence.
; Validated source AST: scale * x[i] + y[i] -> out[i], guarded by i < n.
; The runtime must independently prove buffer identities, nonoverlapping loans,
; device capabilities, launch geometry and fence retirement before dispatch.
target triple = "nvptx64-nvidia-cuda"

declare i32 @llvm.nvvm.read.ptx.sreg.ctaid.x()
declare i32 @llvm.nvvm.read.ptx.sreg.ntid.x()
declare i32 @llvm.nvvm.read.ptx.sreg.tid.x()

define ptx_kernel void @saxpy(float %scale, ptr addrspace(1) %x, ptr addrspace(1) %y, ptr addrspace(1) %out, i64 %n) {
entry:
  %cta = call i32 @llvm.nvvm.read.ptx.sreg.ctaid.x()
  %ntid = call i32 @llvm.nvvm.read.ptx.sreg.ntid.x()
  %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
  %cta64 = zext i32 %cta to i64
  %ntid64 = zext i32 %ntid to i64
  %tid64 = zext i32 %tid to i64
  %base = mul nuw i64 %cta64, %ntid64
  %i = add nuw i64 %base, %tid64
  %inside = icmp ult i64 %i, %n
  br i1 %inside, label %body, label %exit
body:
  %xpos = getelementptr float, ptr addrspace(1) %x, i64 %i
  %ypos = getelementptr float, ptr addrspace(1) %y, i64 %i
  %opos = getelementptr float, ptr addrspace(1) %out, i64 %i
  %vx = load float, ptr addrspace(1) %xpos, align 4
  %vy = load float, ptr addrspace(1) %ypos, align 4
  %scaled = fmul float %scale, %vx
  %sum = fadd float %scaled, %vy
  store float %sum, ptr addrspace(1) %opos, align 4
  br label %exit
exit:
  ret void
}

!nvvm.annotations = !{!0}
!0 = !{ptr @saxpy, !"kernel", i32 1}
)IR";
    return {std::string(nvvm_ir), tokens.size()};
}

} // namespace oreslang::gpu::g1
