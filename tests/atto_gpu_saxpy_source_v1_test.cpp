#include "oreslang/frontend/AttoGpuSaxpyV1.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
void check(bool ok, const char* what) {
    if (!ok) throw std::runtime_error(what);
}
template<typename Callable> void reject(Callable&& call) {
    try { call(); } catch (const std::exception&) { return; }
    throw std::runtime_error("unadmitted GPU source/geometry accepted");
}
std::string replace_once(std::string value,
                         const std::string& from,
                         const std::string& to) {
    const auto at = value.find(from);
    if (at == std::string::npos) throw std::runtime_error("missing test control");
    value.replace(at, from.size(), to);
    return value;
}
}

int main() {
    using namespace oreslang::gpu_source_v1;
    const std::string source = R"atto(
compute fnc saxpy(x: DeviceRead<f32>, y: DeviceRead<f32>,
                  out: DeviceWrite<f32>, alpha: f32, n: u64) device {
  const i = gpu.global_id.x;
  if i < n {
    out[i] = alpha * x[i] + y[i];
  }
}
)atto";
    const auto artifact = compile_saxpy(source);
    check(artifact.kernel_name == "atto_saxpy_f32", "kernel name");
    check(artifact.abi_version == 1 && artifact.required_address_bits == 64
          && artifact.requires_f32, "GPU ABI contract");
    check(artifact.device_source.find("__kernel void atto_saxpy_f32") !=
          std::string::npos, "OpenCL device output");
    check(artifact.device_source.find("if (i >= n) return;") !=
          std::string::npos, "bounds mask");
    check(artifact.device_source.find("alpha * x[i]") !=
          std::string::npos, "SAXPY math");
    check(compile_saxpy("// gpu { arbitrary text }\n" + source).device_source ==
          artifact.device_source, "comments must be inert");
    check(compile_saxpy("/* old kernel */\n" + source).device_source ==
          artifact.device_source, "block comments must be inert");

    const std::vector<std::string> rejected = {
        replace_once(source, "DeviceRead<f32>", "DeviceRead<i64>"),
        replace_once(source, "DeviceWrite<f32>", "DeviceRead<f32>"),
        replace_once(source, "n: u64", "n: int"),
        replace_once(source, "gpu.global_id.x", "gpu.local_id.x"),
        replace_once(source, "i < n", "i > n"),
        replace_once(source, "if i < n {", "if true {"),
        replace_once(source, "out[i] = alpha * x[i] + y[i];",
                     "out[i] = x[i];"),
        replace_once(source, "out[i] = alpha * x[i] + y[i];",
                     "print(i); out[i] = alpha * x[i] + y[i];"),
        replace_once(source, "x[i]", "x[i + 1]"),
        source + "gpu task { 7 }",
        "/* unclosed " + source,
        "/* nested /* comment */\n" + source,
        source + "\n// comment\nfnc host() { return 1; }",
        "saxpy();",
        ""
    };
    for (const auto& invalid : rejected)
        reject([&] { (void)compile_saxpy(invalid); });
    reject([&] { (void)compile_saxpy(std::string(65537, ' ')); });
    reject([&] { (void)compile_saxpy(std::string(600, 'a')); });

    const Limits caps{{65535, 65535, 65535}, {1024, 1024, 64},
                      1024, 100000000, 49152};
    const Launch good{{8, 2, 1}, {256, 1, 1}, 0};
    const auto admitted = validate_launch(good, caps);
    check(admitted.threads_per_block == 256 &&
          admitted.total_threads == 4096, "3D launch multiplication");
    for (const auto& invalid : std::vector<Launch>{
            {{0, 2, 1}, {256, 1, 1}, 0},
            {{65536, 1, 1}, {256, 1, 1}, 0},
            {{8, 1, 1}, {0, 1, 1}, 0},
            {{8, 1, 1}, {1024, 2, 1}, 0},
            {{8, 1, 1}, {256, 1, 1}, 49153},
            {{65535, 65535, 65535}, {1024, 1, 1}, 0}
        }) {
        reject([&] { (void)validate_launch(invalid, caps); });
    }
    const auto max = std::numeric_limits<std::uint64_t>::max();
    const Limits huge{{max, max, max}, {max, max, max},
                      max, max, max};
    reject([&] {
        (void)validate_launch({{max, 2, 1}, {1, 1, 1}, 0}, huge);
    });
    reject([&] {
        (void)validate_launch({{1, 1, 1}, {max, 2, 1}, 0}, huge);
    });
    reject([&] {
        (void)validate_launch({{1, 1, 1}, {1, 1, 1}, 0},
                              {{1, 1, 1}, {1, 1, 1}, 0, 1, 0});
    });

    std::cout << "atto-gpu-saxpy-source-v1: reference admission checks passed\n";
}
