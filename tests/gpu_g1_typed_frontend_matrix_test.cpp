#include "oreslang/backend/GpuG1AttoSaxpySource.hpp"
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using oreslang::gpu::g1::compile_g1_saxpy_source;
int positive = 0;
int negative = 0;
void check(bool ok) { if (!ok) std::abort(); }
std::string replace_once(std::string source, std::string_view from, std::string_view to) {
    const auto i = source.find(from);
    check(i != std::string::npos);
    source.replace(i, from.size(), to);
    return source;
}
void accepted(const std::string& source, const std::string& canonical) {
    const auto checked = compile_g1_saxpy_source(source);
    check(checked.llvm_ir == canonical);
    ++positive;
}
void rejected(const std::string& source) {
    bool did_reject = false;
    try { (void)compile_g1_saxpy_source(source); }
    catch (const std::invalid_argument&) { did_reject = true; }
    check(did_reject);
    ++negative;
}
}
int main() {
    const std::string source = R"ATTO(
      pub gpu fnc saxpy(f32 scale, GpuArray<f32> x, GpuArray<f32> y,
          GpuArray<f32> mut out, u64 n) => void {
        const u64 i = gpu.global_id(0);
        if i >= n { return; }
        out[i] = scale * x[i] + y[i];
      }
    )ATTO";
    const auto canonical = compile_g1_saxpy_source(source).llvm_ir;
    accepted(source, canonical);
    accepted("// source preamble\n" + source, canonical);
    accepted("/* source preamble */\n" + source, canonical);
    // Complete alpha-renaming preserves typed kernel semantics.
    std::string all = source;
    all = replace_once(all, "f32 scale", "f32 alpha");
    all = replace_once(all, "= scale *", "= alpha *");
    accepted(all, canonical);
    std::string names = source;
    names = replace_once(names, "GpuArray<f32> x", "GpuArray<f32> first");
    names = replace_once(names, "x[i]", "first[i]");
    accepted(names, canonical);
    names = source;
    names = replace_once(names, "GpuArray<f32> y", "GpuArray<f32> second");
    names = replace_once(names, "y[i]", "second[i]");
    accepted(names, canonical);
    names = source;
    names = replace_once(names, "mut out", "mut destination");
    names = replace_once(names, "out[i]", "destination[i]");
    accepted(names, canonical);
    names = source;
    names = replace_once(names, "u64 n", "u64 count");
    names = replace_once(names, "i >= n", "i >= count");
    accepted(names, canonical);
    names = source;
    names = replace_once(names, "u64 i", "u64 lane");
    names = replace_once(names, "if i >=", "if lane >=");
    names = replace_once(names, "out[i]", "out[lane]");
    names = replace_once(names, "x[i]", "x[lane]");
    names = replace_once(names, "y[i]", "y[lane]");
    accepted(names, canonical);
    accepted(replace_once(source, "out[i] = scale * x[i] + y[i];", "out[i] = scale * x[i] + y[i]; return;"), canonical);
    accepted("\n\t" + source + "\n\t", canonical);
    accepted("/*safe*/" + source + "//safe end\n", canonical);

    const std::vector<std::pair<std::string, std::string>> mutations = {
        {"pub gpu", "gpu"}, {"pub gpu", "pub async gpu"},
        {"fnc saxpy", "fnc hack"}, {"saxpy(", "saxpy2("},
        {"f32 scale", "f64 scale"}, {"f32 scale", "f32 x"},
        {"f32 scale", "f32 gpu"}, {"f32 scale", "f32 3"},
        {"GpuArray<f32> x", "GpuArray<f64> x"},
        {"GpuArray<f32> x", "HostBuffer<f32> x"},
        {"GpuArray<f32> y", "HostBuffer<f32> y"},
        {"GpuArray<f32> y", "GpuArray<f32> mut y"},
        {"GpuArray<f32> y", "GpuArray<f32> x"},
        {"GpuArray<f32> mut out", "GpuArray<f32> out"},
        {"GpuArray<f32> mut out", "GpuArray<f32> mut y"},
        {"u64 n", "i64 n"}, {"u64 n", "u64 out"},
        {"u64 n", "u64 return"},
        {") => void", ") -> void"}, {") => void", ") => i64"},
        {"const u64 i", "let u64 i"},
        {"const u64 i", "const u32 i"},
        {"const u64 i", "const u64 out"},
        {"const u64 i =", "u64 i ="},
        {"gpu.global_id(0)", "gpu.global_id(1)"},
        {"gpu.global_id(0)", "gpu.global_id(n)"},
        {"gpu.global_id(0)", "cpu.global_id(0)"},
        {"gpu.global_id(0)", "gpu.global_id(0,0)"},
        {"if i >= n", "if i > n"},
        {"if i >= n", "if n >= i"},
        {"if i >= n", "if i >= x"},
        {"if i >= n", "if i <= n"},
        {"return; }", "continue; }"},
        {"return; }", "await(); }"},
        {"return; }", "panic(); }"},
        {"out[i] =", "x[i] ="},
        {"out[i] =", "out[n] ="},
        {"out[i] =", "out[i + 1] ="},
        {"scale * x[i]", "scale + x[i]"},
        {"scale * x[i]", "x[i] * scale"},
        {"scale * x[i]", "scale * y[i]"},
        {"scale * x[i]", "scale * x[n]"},
        {"+ y[i]", "+ y[n]"},
        {"+ y[i]", "- y[i]"},
        {"+ y[i]", "+ y[i] + 1"},
        {"= scale *", "= 0 *"},
        {"= scale *", "= host()"},
        {"const u64 i", "const u64 j"},
        {"out[i] = scale", "gpu.group_barrier(); out[i] = scale"},
        {"out[i] = scale", "critical { } out[i] = scale"},
        {"out[i] = scale", "rt take out; out[i] = scale"},
        {"out[i] = scale", "return; out[i] = scale"},
        {"out[i] = scale", "while true {} out[i] = scale"},
        {"out[i] = scale", "suspends fnc host() {} out[i] = scale"},
        {"out[i] = scale", "await sleep(1); out[i] = scale"},
        {"out[i] = scale", "new Actor(); out[i] = scale"},
        {"out[i] = scale", "spawn task(); out[i] = scale"},
        {"out[i] = scale", "gpu.subgroup_barrier(); out[i] = scale"},
        {"if i >= n {", "if i >= n { if true {"},
        {"if i >= n { return; }", ""},
        {"scale * x[i] + y[i]", "scale * x[i] + y[i] + y[i]"}
    };
    for (const auto& [before, after] : mutations)
        rejected(replace_once(source, before, after));
    rejected(source + "pub fnc host() => void {}");
    rejected(source + "launch saxpy(scale, x, y, out, n) on gpu(device) {}");
    rejected(source + ";");
    rejected(source + "/* unterminated");
    rejected(source + "\x01");
    rejected("");
    rejected(std::string(65537, 'x'));
    rejected("/* nested /* forbidden */ */" + source);
    rejected(replace_once(source, "const u64 i", "const u64 i/*oops*/i"));
    rejected(replace_once(source, "const u64 i", "const u64 1i"));
    std::cout << "PASS positive=" << positive << " negative=" << negative
              << " GPU-G1 typed frontend alpha-renaming/mutation checks (HOST ONLY)\n";
}
