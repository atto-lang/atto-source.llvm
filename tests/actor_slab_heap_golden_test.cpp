// Cross-backend contract test: replays the op script and compares every
// word against tests/fixtures/actor_slab_heap_golden_v1.txt. The Rust/wasm32
// heap (atto-source.rs src/actor_slab_heap.rs) checks the same fixture.
#include "oreslang/runtime/ActorSlabHeap.hpp"
#include <cstdio>
#include <fstream>
#include <vector>
using namespace oreslang::runtime;
int main(int argc, char** argv) {
    if (argc != 2) { std::fprintf(stderr, "usage: %s GOLDEN.txt\n", argv[0]); return 2; }
    ActorSlabHeapConfig c; c.reserve_bytes = 16u << 20; c.budget_bytes = 16u << 20;
    c.commit_granule = 64 * 1024; c.handle_capacity = 64;
    ActorSlabHeap h(c);
    if (!h.ok()) return 2;
    std::uint32_t lcg = 7;
    auto next = [&] { lcg = lcg * 1664525u + 1013904223u; return lcg >> 8; };
    const std::size_t sizes[] = {8, 16, 24, 40, 64, 100, 200, 512, 1000, 2048, 3000, 9000};
    const std::size_t aligns[] = {8, 16, 32, 64};
    std::vector<ActorSlabHeap::Ref> live; std::vector<ActorSlabHeap::Handle32> handles;
    std::vector<std::uint32_t> out;
    for (int op = 0; op < 2000; ++op) {
        const auto v = next() % 10;
        if (v <= 4) {
            // Sequence the two draws explicitly: argument order is unspecified.
            const auto s = sizes[next() % 12]; const auto a = aligns[next() % 4];
            auto r = h.allocate(s, a); if (!r) return 3;
            out.push_back(r.value.storage()); live.push_back(r.value);
        } else if ((v == 5 || v == 6) && !live.empty()) {
            const auto i = next() % live.size();
            if (h.free(live[i]) != SlabStatus::Ok) return 4;
            live[i] = live.back(); live.pop_back(); out.push_back(0);
        } else if (v == 7) {
            auto x = h.allocate_handle(sizes[next() % 6]);
            if (x) { out.push_back(x.value.word); out.push_back(h.resolve_handle(x.value).value.storage()); handles.push_back(x.value); }
        } else if (v == 8 && !handles.empty()) {
            const auto i = next() % handles.size();
            if (h.free_handle(handles[i]) != SlabStatus::Ok) return 5;
            handles[i] = handles.back(); handles.pop_back(); out.push_back(1);
        }
    }
    std::ifstream golden(argv[1]);
    std::vector<std::uint32_t> expected; std::uint32_t w;
    while (golden >> w) expected.push_back(w);
    if (expected.size() != out.size()) { std::fprintf(stderr, "FAIL golden length %zu != %zu\n", out.size(), expected.size()); return 1; }
    for (std::size_t i = 0; i < out.size(); ++i)
        if (out[i] != expected[i]) { std::fprintf(stderr, "FAIL golden word %zu: %u != %u\n", i, out[i], expected[i]); return 1; }
    std::printf("PASS actor slab heap golden v1: %zu words, %zu-bit host\n", out.size(), sizeof(void*) * 8);
    return 0;
}
