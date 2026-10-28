// Trusted native C++ ↔ WASM host pipe conformance peer. NOT the complete
// native OresVM actor runtime or an authenticated remote actor protocol.
// Uses the real native MessagePackWireV2 implementation, not a JS mock.
#include "../src/main/cpp/oreslang/runtime/MessagePackWireV2.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <exception>
#include <stdexcept>
#include <vector>
#include <string>
#include <limits>

namespace {
using namespace oreslang::wire;
constexpr std::size_t kMaxFrame = 4096;
const Shape kPair = Shape::tuple({Shape::integer(), Shape::integer()});
constexpr const char *kRequest = "oreslang.native_wasm.data.request.v0";
constexpr const char *kReply = "oreslang.native_wasm.data.response.v0";
constexpr const char *kAck = "oreslang.native_wasm.data.ack.v0";

void read_exact(unsigned char* out, std::size_t len) {
    while (len) {
        const std::size_t n = std::fread(out, 1, len, stdin);
        if (!n) throw std::runtime_error("native peer input truncated");
        out += n;
        len -= n;
    }
}
void write_exact(const unsigned char* p, std::size_t len) {
    while (len) {
        const std::size_t n = std::fwrite(p, 1, len, stdout);
        if (!n) throw std::runtime_error("native peer output failure");
        p += n;
        len -= n;
    }
}
void write_frame(const char* schema, std::int64_t ticket, std::int64_t result) {
    const auto bytes = encode(schema, kPair, Value(Value::Items{Value(ticket), Value(result)}));
    if (bytes.empty() || bytes.size() > kMaxFrame) throw std::runtime_error("frame budget exceeded");
    const auto n = static_cast<std::uint32_t>(bytes.size());
    const std::array<unsigned char,4> length = {
        static_cast<unsigned char>(n >> 24), static_cast<unsigned char>(n >> 16),
        static_cast<unsigned char>(n >> 8), static_cast<unsigned char>(n)
    };
    write_exact(length.data(), length.size());
    write_exact(bytes.data(), bytes.size());
    if (std::fflush(stdout) != 0) throw std::runtime_error("native peer flush failure");
}
std::pair<std::int64_t, std::int64_t> read_frame(const char* schema) {
    std::array<unsigned char,4> length{};
    read_exact(length.data(), length.size());
    std::uint32_t n = (static_cast<std::uint32_t>(length[0])<<24) |
        (static_cast<std::uint32_t>(length[1])<<16) |
        (static_cast<std::uint32_t>(length[2])<<8) | length[3];
    if (n == 0 || n > kMaxFrame) throw std::runtime_error("invalid native peer frame length");
    std::vector<unsigned char> bytes(n);
    read_exact(bytes.data(), bytes.size());
    const auto result = decode(schema, kPair, bytes);
    const auto& pair = std::get<Value::Items>(result.payload);
    if (pair.size() != 2) throw std::runtime_error("invalid native wire pair");
    return {std::get<std::int64_t>(pair[0].payload),
            std::get<std::int64_t>(pair[1].payload)};
}
int positive_arg(const char* input, int maximum) {
    char* end = nullptr;
    const long n = std::strtol(input, &end, 10);
    if (end == input || *end || n < 1 || n > maximum)
        throw std::runtime_error("invalid native peer argument");
    return static_cast<int>(n);
}
}
int main(int argc, char** argv) {
    try {
        if (argc != 3) throw std::runtime_error("usage: peer <turns> <actor_count>");
        const int turns = positive_arg(argv[1], 10000);
        const int actors = positive_arg(argv[2], 40);
        for (std::int64_t ticket=1; ticket <= turns; ++ticket) {
            // Native → WASM: one trusted host-owned DATA request.
            write_frame(kRequest, ticket, 1);
            // WASM → Native: canonical typed response for the same ticket.
            const auto [received, result] = read_frame(kReply);
            if (received != ticket || result != (ticket - 1) / actors + 1)
                throw std::runtime_error("wrong ticket, duplicate, or actor state divergence");
            // Native → WASM host: proof of native computation, not just echo.
            write_frame(kAck, ticket, result + 100);
        }
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "NATIVE_WASM_WIRE_REJECTED: %s\n", e.what());
        return 1;
    }
}
