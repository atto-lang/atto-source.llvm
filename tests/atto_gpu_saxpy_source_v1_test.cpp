#include "oreslang/frontend/AttoGpuSaxpyV1.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <utility>
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

    // Each of these single-edit guest-source mutations must fail before
    // emitting any device artifact. This intentionally closed parser does
    // not admit host closures, hidden effects, alternative indexing, or
    // accidental CUDA/C++ syntax by permissive token fallback.
    const std::vector<std::pair<std::string, std::string>> syntax_mutations = {
        {"compute fnc saxpy", "gpu fnc saxpy"},
        {"compute fnc saxpy", "suspends compute fnc saxpy"},
        {"compute fnc saxpy", "compute routine saxpy"},
        {"saxpy(", "saxpy_untrusted("},
        {"x: DeviceRead<f32>", "x: DeviceWrite<f32>"},
        {"y: DeviceRead<f32>", "y: DeviceWrite<f32>"},
        {"out: DeviceWrite<f32>", "out: DeviceRead<f32>"},
        {"alpha: f32", "alpha: f64"},
        {"n: u64", "n: u32"},
        {") device {", ") host {"},
        {"const i =", "let i ="},
        {"const i =", "const j ="},
        {"gpu.global_id.x", "gpu.global_id.y"},
        {"gpu.global_id.x", "gpu.global_id.z"},
        {"gpu.global_id.x", "gpu.local_id.x"},
        {"gpu.global_id.x", "gpu.global_id.x + 1"},
        {"if i < n", "if i <= n"},
        {"if i < n", "if n > i"},
        {"if i < n", "if i < n || true"},
        {"out[i]", "x[i]"},
        {"out[i]", "out[n]"},
        {"x[i]", "x[n]"},
        {"y[i]", "y[n]"},
        {"alpha * x[i]", "x[i] * alpha"},
        {"alpha * x[i]", "alpha * y[i]"},
        {"alpha * x[i]", "alpha * x[i] * x[i]"},
        {"out[i] = alpha", "out[i] += alpha"},
        {"out[i] = alpha", "await out[i] = alpha"},
        {"out[i] = alpha", "atomic out[i] = alpha"},
        {"if i < n {", "if i < n { print(i);"},
        {"if i < n {", "if i < n { barrier();"},
        {"if i < n {", "if i < n { gpu.shared();"},
        {"compute fnc saxpy", "pub compute fnc saxpy"},
        {"gpu.global_id.x", "gpu.global_id.x; host_call()"}
    };
    for (const auto& mutation : syntax_mutations) {
        reject([&] {
            (void)compile_saxpy(replace_once(
                source, mutation.first, mutation.second));
        });
    }
    // Lexical normalization is not syntax expansion; whitespace and
    // comments may be inert, but embedded binary operators/directives,
    // arbitrary declarations and control bytes still fail closed.
    reject([&] { (void)compile_saxpy(source + std::string(1, '\0')); });
    reject([&] { (void)compile_saxpy(source + "\nfnc unsafe(){}"); });
    reject([&] { (void)compile_saxpy(
        "compute fnc saxpy(x: DeviceRead<f32>,\n" + source); });
    check(compile_saxpy("\n\t/* no capabilities */\n" + source +
          "\n// host injected in comment is inert\n").device_source ==
          artifact.device_source, "valid comments/whitespace preserve artifact");

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
    // Exhaustive small-grid property gate: each valid dimension triple
    // must preserve exact multiplication. Reject one-boundary violations
    // even when another dimension is small enough to fit in total quota.
    const Limits compact{{4, 3, 2}, {8, 4, 2}, 32, 512, 64};
    std::uint32_t valid_geometry_cases = 0;
    for (std::uint64_t gx = 1; gx <= 4; ++gx) {
        for (std::uint64_t gy = 1; gy <= 3; ++gy) {
            for (std::uint64_t gz = 1; gz <= 2; ++gz) {
                for (std::uint64_t bx : {1u, 2u, 4u, 8u}) {
                    for (std::uint64_t by : {1u, 2u, 4u}) {
                        for (std::uint64_t bz : {1u, 2u}) {
                            const std::uint64_t block = bx * by * bz;
                            const std::uint64_t total = gx * gy * gz * block;
                            Launch candidate{{gx, gy, gz},
                                             {bx, by, bz}, 0};
                            if (block <= compact.max_block_threads &&
                                total <= compact.max_launch_threads) {
                                const auto valid = validate_launch(candidate, compact);
                                check(valid.threads_per_block == block &&
                                      valid.total_threads == total,
                                      "3D geometry property exact multiplication");
                                ++valid_geometry_cases;
                            } else {
                                reject([&] { (void)validate_launch(candidate, compact); });
                            }
                        }
                    }
                }
            }
        }
    }
    check(valid_geometry_cases > 100,
          "geometry matrix must cover many nontrivial 3D configurations");
    reject([&] { (void)validate_launch(
        {{5, 1, 1}, {1, 1, 1}, 0}, compact); });
    reject([&] { (void)validate_launch(
        {{1, 4, 1}, {1, 1, 1}, 0}, compact); });
    reject([&] { (void)validate_launch(
        {{1, 1, 3}, {1, 1, 1}, 0}, compact); });
    reject([&] { (void)validate_launch(
        {{1, 1, 1}, {9, 1, 1}, 0}, compact); });
    reject([&] { (void)validate_launch(
        {{1, 1, 1}, {1, 5, 1}, 0}, compact); });
    reject([&] { (void)validate_launch(
        {{1, 1, 1}, {1, 1, 3}, 0}, compact); });
    reject([&] { (void)validate_launch(
        {{1, 1, 1}, {1, 1, 1}, 65}, compact); });

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
