#pragma once

#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace oreslang::gpu_source_v1 {

// A deliberately small, separately enabled source-compiler slice.
// It accepts exactly one well-typed, bounds-masked SAXPY declaration in .atto.
// The output is OpenCL C source, NOT a dispatched kernel or a trusted device claim.
// General gpu blocks/tasks remain rejected by the production scalar frontend.
struct Artifact {
    std::string kernel_name;
    std::string device_source;
    std::uint32_t abi_version;
    std::uint32_t required_address_bits;
    bool requires_f32;
};

class SaxpyParser final {
public:
    explicit SaxpyParser(std::string_view source) : tokens_(scan(source)) {}

    void parse() {
        expect("compute"); expect("fnc"); expect("saxpy"); expect("(");
        parameter("x", "DeviceRead", true);
        expect(",");
        parameter("y", "DeviceRead", true);
        expect(",");
        parameter("out", "DeviceWrite", true);
        expect(",");
        parameter("alpha", "f32", false);
        expect(",");
        parameter("n", "u64", false);
        expect(")"); expect("device"); expect("{");
        expect("const"); expect("i"); expect("="); expect("gpu");
        expect("."); expect("global_id"); expect("."); expect("x"); expect(";");
        expect("if"); expect("i"); expect("<"); expect("n"); expect("{");
        expect("out"); expect("["); expect("i"); expect("]"); expect("=");
        expect("alpha"); expect("*"); expect("x"); expect("["); expect("i");
        expect("]"); expect("+"); expect("y"); expect("["); expect("i");
        expect("]"); expect(";");
        expect("}"); expect("}");
        if (at_ != tokens_.size())
            throw std::invalid_argument("GPU SAXPY v1 rejects trailing declarations or effects");
    }

private:
    static bool start(char ch) noexcept {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_';
    }
    static bool part(char ch) noexcept {
        return start(ch) || (ch >= '0' && ch <= '9');
    }
    static std::vector<std::string_view> scan(std::string_view source) {
        if (source.empty() || source.size() > 65536)
            throw std::invalid_argument("GPU SAXPY v1 source must be between 1 and 65536 bytes");
        std::vector<std::string_view> tokens;
        tokens.reserve(96);
        std::size_t pos = 0;
        while (pos < source.size()) {
            const unsigned char ch = static_cast<unsigned char>(source[pos]);
            if (ch == ' ' || ch == '\n' || ch == '\t' || ch == '\r') {
                ++pos;
                continue;
            }
            if (pos + 1 < source.size() && source.substr(pos, 2) == "//") {
                const auto end = source.find('\n', pos + 2);
                pos = end == std::string_view::npos ? source.size() : end;
                continue;
            }
            if (pos + 1 < source.size() && source.substr(pos, 2) == "/*") {
                const auto end = source.find("*/", pos + 2);
                if (end == std::string_view::npos)
                    throw std::invalid_argument("GPU SAXPY v1 unterminated block comment");
                // Nested comments are not admitted in this restricted profile.
                if (source.substr(pos + 2, end - pos - 2).find("/*") != std::string_view::npos)
                    throw std::invalid_argument("GPU SAXPY v1 nested block comment unsupported");
                pos = end + 2;
                continue;
            }
            const std::size_t begin = pos;
            if (start(static_cast<char>(ch))) {
                while (pos < source.size() && part(source[pos])) ++pos;
            } else if (ch >= '0' && ch <= '9') {
                while (pos < source.size() && source[pos] >= '0' && source[pos] <= '9') ++pos;
            } else if (std::string_view("(){}[]<>,:;=+*.").find(static_cast<char>(ch))
                       != std::string_view::npos) {
                ++pos;
            } else {
                throw std::invalid_argument("GPU SAXPY v1 unsupported lexical token");
            }
            if (tokens.size() == 256)
                throw std::invalid_argument("GPU SAXPY v1 token budget exceeded");
            tokens.emplace_back(source.substr(begin, pos - begin));
        }
        return tokens;
    }

    void expect(std::string_view expected) {
        if (at_ >= tokens_.size() || tokens_[at_] != expected)
            throw std::invalid_argument("GPU SAXPY v1 requires exact typed/bounded kernel syntax");
        ++at_;
    }
    void parameter(std::string_view name, std::string_view type, bool device_view) {
        expect(name); expect(":"); expect(type);
        if (device_view) { expect("<"); expect("f32"); expect(">"); }
    }

    std::vector<std::string_view> tokens_;
    std::size_t at_ = 0;
};

inline Artifact compile_saxpy(std::string_view source) {
    SaxpyParser parser(source);
    parser.parse();
    // No source identifier or expression is interpolated in device code.
    // The complete validated device body is lowered from a closed AST shape.
    return Artifact{
        "atto_saxpy_f32",
        "#pragma OPENCL FP_CONTRACT OFF\n"
        "__kernel void atto_saxpy_f32(__global const float* x,\n"
        "                               __global const float* y,\n"
        "                               __global float* out,\n"
        "                               const float alpha, const ulong n) {\n"
        "  const ulong i = (ulong)get_global_id(0);\n"
        "  if (i >= n) return;\n"
        "  out[i] = (alpha * x[i]) + y[i];\n"
        "}\n",
        1, 64, true
    };
}

// Host-side checked launch geometry only. Hardware/device-specific limits
// are supplied by a trusted provider; this is NOT dispatch authorization.
struct Launch {
    std::array<std::uint64_t, 3> grid;
    std::array<std::uint64_t, 3> block;
    std::uint64_t dynamic_shared_bytes;
};
struct Limits {
    std::array<std::uint64_t, 3> max_grid_axes;
    std::array<std::uint64_t, 3> max_block_axes;
    std::uint64_t max_block_threads;
    std::uint64_t max_launch_threads;
    std::uint64_t max_dynamic_shared_bytes;
};
struct LaunchResult {
    std::uint64_t threads_per_block;
    std::uint64_t total_threads;
};
inline std::uint64_t checked_product(std::uint64_t left, std::uint64_t right) {
    if (right != 0 && left > std::numeric_limits<std::uint64_t>::max() / right)
        throw std::overflow_error("GPU launch dimension product overflow");
    return left * right;
}
inline LaunchResult validate_launch(const Launch& launch, const Limits& limits) {
    std::uint64_t groups = 1;
    std::uint64_t block_threads = 1;
    for (std::size_t i = 0; i < 3; ++i) {
        if (launch.grid[i] == 0 || limits.max_grid_axes[i] == 0
                || launch.grid[i] > limits.max_grid_axes[i]
                || launch.block[i] == 0 || limits.max_block_axes[i] == 0
                || launch.block[i] > limits.max_block_axes[i])
            throw std::invalid_argument("GPU launch axes outside trusted device limits");
        groups = checked_product(groups, launch.grid[i]);
        block_threads = checked_product(block_threads, launch.block[i]);
    }
    if (limits.max_block_threads == 0 || limits.max_launch_threads == 0
            || block_threads > limits.max_block_threads
            || launch.dynamic_shared_bytes > limits.max_dynamic_shared_bytes)
        throw std::invalid_argument("GPU launch workgroup/shared-memory quota exceeded");
    const auto total = checked_product(groups, block_threads);
    if (total > limits.max_launch_threads)
        throw std::invalid_argument("GPU launch total thread quota exceeded");
    return {block_threads, total};
}

} // namespace oreslang::gpu_source_v1
