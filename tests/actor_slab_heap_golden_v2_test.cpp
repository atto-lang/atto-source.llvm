// Cross-backend contract v2: failure paths and edge cases, not just words.
//
// Same deterministic op script in C++ (here), Rust (atto-source.rs),
// C# (atto-source.net) and Java (atto-source.java). Each language maps its
// status enum to the CANONICAL codes below, so errors must agree too.
// Output word per op (unsigned):
//   success with a word         -> the word (ref32 or Handle32)
//   successful free/free_handle -> 0
//   failure                     -> 0x80000000 | canonical code
// followed by 5 stats words: live_objects, live_pages, free_pages,
// quarantined_handles, committed_bytes.
//
// usage: actor_slab_heap_golden_v2_test GOLDEN.txt   (compare)
//        actor_slab_heap_golden_v2_test --emit       (print fixture)
#include "oreslang/runtime/ActorSlabHeap.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

using namespace oreslang::runtime;

// Canonical status codes shared by every backend.
static std::uint32_t code(SlabStatus s) {
    switch (s) {
        case SlabStatus::Ok: return 0;
        case SlabStatus::OutOfMemory: return 1;
        case SlabStatus::BudgetExceeded: return 2;
        case SlabStatus::InvalidSize: return 3;
        case SlabStatus::InvalidAlignment: return 4;
        case SlabStatus::InvalidRef: return 5;
        case SlabStatus::StaleHandle: return 6;
        case SlabStatus::HandleTableFull: return 7;
        case SlabStatus::CommitFailed: return 1; // an OS commit refusal is out-of-memory
        case SlabStatus::Retired: return 8;
    }
    return 99;
}
static std::uint32_t fail(SlabStatus s) { return 0x80000000u | code(s); }

static std::vector<std::uint32_t> golden_script_v2() {
    ActorSlabHeapConfig c;
    c.reserve_bytes = 256 * 1024;   // 64 pages
    c.budget_bytes = 128 * 1024;    // budget binds before the reservation
    c.commit_granule = 16 * 1024;
    c.handle_capacity = 8;          // 4 slot bits
    c.handle_generation_bits = 3;   // generations 1..7, then quarantine
    ActorSlabHeap h(c);
    std::vector<std::uint32_t> out;
    if (!h.ok()) return out;
    std::uint32_t lcg = 99;
    auto next = [&] { lcg = lcg * 1664525u + 1013904223u; return lcg >> 8; };
    const std::size_t sizes[] = {0, 1, 8, 16, 17, 33, 48, 63, 64, 65, 100, 127, 128, 129,
                                 255, 256, 257, 500, 1000, 2047, 2048, 2049};
    const std::size_t aligns[] = {1, 8, 16, 32, 64, 128, 3};
    const std::size_t larges[] = {3000, 4033, 4032, 8000, 12000, 20000};
    std::vector<std::uint32_t> live, freed, handles, freed_handles;
    auto remember = [](std::vector<std::uint32_t>& v, std::uint32_t w) {
        v.push_back(w);
        if (v.size() > 8) v.erase(v.begin());
    };
    for (int op = 0; op < 3000; ++op) {
        const auto v = next() % 16;
        if (v <= 5) {
            const auto s = sizes[next() % 22];
            const auto a = aligns[next() % 7];
            auto r = h.allocate(s, a);
            if (r) { out.push_back(r.value.storage()); live.push_back(r.value.storage()); }
            else out.push_back(fail(r.status));
        } else if (v <= 8) {
            if (live.empty()) continue;
            const auto i = next() % live.size();
            const auto w = live[i];
            const auto st = h.free(ActorSlabHeap::Ref::from_storage(w));
            out.push_back(st == SlabStatus::Ok ? 0 : fail(st));
            live[i] = live.back(); live.pop_back();
            remember(freed, w);
        } else if (v == 9) {
            // Stale/ABA free: may now name a reallocated block. Deterministic either way.
            if (freed.empty()) continue;
            const auto w = freed[next() % freed.size()];
            const auto st = h.free(ActorSlabHeap::Ref::from_storage(w));
            out.push_back(st == SlabStatus::Ok ? 0 : fail(st));
            if (st == SlabStatus::Ok)
                for (auto& l : live) if (l == w) { l = live.back(); live.pop_back(); break; }
        } else if (v <= 11) {
            auto x = h.allocate_handle(sizes[1 + next() % 12]);
            if (x) { out.push_back(x.value.word); handles.push_back(x.value.word); }
            else out.push_back(fail(x.status));
        } else if (v <= 13) {
            if (handles.empty()) continue;
            const auto i = next() % handles.size();
            const auto w = handles[i];
            const auto st = h.free_handle(ActorSlabHeap::Handle32{w});
            out.push_back(st == SlabStatus::Ok ? 0 : fail(st));
            handles[i] = handles.back(); handles.pop_back();
            remember(freed_handles, w);
        } else if (v == 14) {
            if (freed_handles.empty()) continue;
            auto r = h.resolve_handle(ActorSlabHeap::Handle32{freed_handles[next() % freed_handles.size()]});
            out.push_back(r ? r.value.storage() : fail(r.status));
        } else {
            auto r = h.allocate(larges[next() % 6]);
            if (r) { out.push_back(r.value.storage()); live.push_back(r.value.storage()); }
            else out.push_back(fail(r.status));
        }
    }
    const auto& st = h.stats();
    out.push_back(static_cast<std::uint32_t>(st.live_objects));
    out.push_back(static_cast<std::uint32_t>(st.live_pages));
    out.push_back(static_cast<std::uint32_t>(st.free_pages));
    out.push_back(st.quarantined_handles);
    out.push_back(static_cast<std::uint32_t>(st.committed_bytes));
    return out;
}

int main(int argc, char** argv) {
    const auto out = golden_script_v2();
    if (out.empty()) { std::fprintf(stderr, "FAIL heap config\n"); return 2; }
    if (argc == 2 && std::strcmp(argv[1], "--emit") == 0) {
        for (auto w : out) std::printf("%u\n", w);
        return 0;
    }
    if (argc != 2) { std::fprintf(stderr, "usage: %s GOLDEN.txt | --emit\n", argv[0]); return 2; }
    std::ifstream golden(argv[1]);
    std::vector<std::uint32_t> expected;
    std::uint32_t w;
    while (golden >> w) expected.push_back(w);
    if (expected.size() != out.size()) {
        std::fprintf(stderr, "FAIL golden v2 length %zu != %zu\n", out.size(), expected.size());
        return 1;
    }
    for (std::size_t i = 0; i < out.size(); ++i)
        if (out[i] != expected[i]) {
            std::fprintf(stderr, "FAIL golden v2 word %zu: %u != %u\n", i, out[i], expected[i]);
            return 1;
        }
    std::printf("PASS actor slab heap golden v2: %zu words, %zu-bit host\n", out.size(), sizeof(void*) * 8);
    return 0;
}
