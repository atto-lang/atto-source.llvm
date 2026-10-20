#include "oreslang/runtime/ManagedRef.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

struct alignas(8) Node { std::uint64_t payload; };
constexpr std::size_t kNodes = 1u << 17;
constexpr std::size_t kSteps = kNodes * 8;

template<oreslang::runtime::ManagedRefMode Mode>
void bench(const std::vector<Node>& nodes) {
    using Ref = oreslang::runtime::ActorManagedRef<Mode>;
    std::vector<Ref> edges;
    edges.reserve(kNodes);
    const auto base = reinterpret_cast<std::uintptr_t>(nodes.data());
    const auto bytes = nodes.size() * sizeof(Node);
    for (std::size_t i = 0; i < kNodes; ++i) {
        const std::size_t target = (i * 8191u) & (kNodes - 1u);
        auto r = Ref::encode(base, bytes, base + target * sizeof(Node));
        if (!r) throw std::runtime_error("failed to encode actor-local benchmark reference");
        edges.push_back(*r);
    }

    const auto exercise = [&]() {
        std::uint64_t sum = 0;
        for (std::size_t i = 0; i < kSteps; ++i) {
            const Ref edge = edges[i & (kNodes - 1u)];
            const auto address = edge.decode(base, bytes);
            if (!address || *address == 0)
                throw std::runtime_error("actor-local benchmark decode unexpectedly failed");
            sum += reinterpret_cast<const Node*>(*address)->payload;
        }
        return sum;
    };

    const std::uint64_t warmup = exercise();
    std::uint64_t checksum = 0;
    const auto start = std::chrono::steady_clock::now();
    for (int trial = 0; trial < 5; ++trial) checksum += exercise();
    const auto end = std::chrono::steady_clock::now();
    if (checksum != warmup * 5)
        throw std::runtime_error("actor-local benchmark numerical result differs between trials");
    const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    std::cout << (Mode == oreslang::runtime::ManagedRefMode::Compressed32 ? "compressed32" : "native64")
              << ",reference_bytes=" << sizeof(Ref)
              << ",edge_storage_bytes=" << edges.capacity() * sizeof(Ref)
              << ",traversed_edges=" << kSteps * 5
              << ",wall_ns=" << nanos
              << ",checksum=" << checksum << '\n';
}

} // namespace

int main() {
    std::vector<Node> nodes(kNodes);
    for (std::size_t i = 0; i < kNodes; ++i) nodes[i].payload = i + 1;
    std::cout << "representation,metrics (isolated host microbenchmark; not source actor AOT)\n";
    bench<oreslang::runtime::ManagedRefMode::Compressed32>(nodes);
    bench<oreslang::runtime::ManagedRefMode::Native64>(nodes);
}
