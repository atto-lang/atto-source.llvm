// ActorSlabHeap: bounded size-class actor allocator, Offset32 + Handle32.
// Runs on 64-bit and 32-bit hosts (see tests/native_heap_pointer_width_gate.rs).
#include "oreslang/runtime/ActorSlabHeap.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

using oreslang::runtime::ActorSlabHeap;
using oreslang::runtime::ActorSlabHeapConfig;
using oreslang::runtime::CompressedActorRef;
using oreslang::runtime::SlabStatus;
using Ref = ActorSlabHeap::Ref;

static_assert(sizeof(Ref) == 4, "guest Offset32 references are 4 bytes on every host");
static_assert(sizeof(ActorSlabHeap::Handle32) == 4, "guest Handle32 references are 4 bytes on every host");

static int checks = 0;
#define REQUIRE(cond) do { ++checks; if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); std::exit(1); } } while (0)

static ActorSlabHeapConfig config(std::size_t reserve, std::size_t budget,
                                  std::uint32_t handles = 0, std::uint32_t gen_bits = 0) {
    ActorSlabHeapConfig c;
    c.reserve_bytes = reserve;
    c.budget_bytes = budget;
    c.commit_granule = 64 * 1024;
    c.handle_capacity = handles;
    c.handle_generation_bits = gen_bits;
    return c;
}

static Ref forge(std::uint32_t word) {
    return Ref::from_storage(word);
}

static void basic_and_classes() {
    ActorSlabHeap heap(config(1 << 20, 1 << 20));
    REQUIRE(heap.ok());
    REQUIRE(heap.stats().committed_bytes == 0); // nothing committed up front
    auto a = heap.allocate(24);
    REQUIRE(a);
    auto p = heap.resolve(a.value, 24);
    REQUIRE(p);
    for (int i = 0; i < 24; ++i) REQUIRE(static_cast<unsigned char*>(p.value)[i] == 0);
    std::memset(p.value, 0xAB, 24);
    REQUIRE(heap.resolve(a.value, 32));                   // 24 -> 32-byte class
    REQUIRE(heap.resolve(a.value, 33).status == SlabStatus::InvalidSize);
    REQUIRE(heap.free(a.value) == SlabStatus::Ok);
    REQUIRE(heap.free(a.value) == SlabStatus::InvalidRef); // double free
    REQUIRE(heap.resolve(a.value, 1).status == SlabStatus::InvalidRef);

    REQUIRE(heap.allocate(0).status == SlabStatus::InvalidSize);
    REQUIRE(heap.allocate(8, 3).status == SlabStatus::InvalidAlignment);
    REQUIRE(heap.allocate(8, 128).status == SlabStatus::InvalidAlignment);
    for (std::size_t align : {8u, 16u, 32u, 64u}) {
        for (std::size_t size : {1u, 17u, 50u, 100u, 700u, 2048u}) {
            auto r = heap.allocate(size, align);
            REQUIRE(r);
            auto q = heap.resolve(r.value, size);
            REQUIRE(q);
            REQUIRE(reinterpret_cast<std::uintptr_t>(q.value) % align == 0);
            REQUIRE(heap.free(r.value) == SlabStatus::Ok);
        }
    }
    REQUIRE(heap.stats().live_objects == 0);
}

static void page_packing_and_reuse() {
    ActorSlabHeap heap(config(1 << 20, 1 << 20));
    std::vector<Ref> refs;
    for (int i = 0; i < 252; ++i) refs.push_back(heap.allocate(16).value);
    REQUIRE(heap.stats().live_pages == 1);      // 252 x 16 B fit one 4 KiB page
    refs.push_back(heap.allocate(16).value);
    REQUIRE(heap.stats().live_pages == 2);
    const auto committed = heap.stats().committed_bytes;
    for (Ref r : refs) REQUIRE(heap.free(r) == SlabStatus::Ok);
    REQUIRE(heap.stats().live_pages == 0);
    REQUIRE(heap.stats().free_pages == 2);
    // Freed pages serve OTHER size classes without committing more memory.
    std::vector<Ref> big;
    for (int i = 0; i < 4; ++i) big.push_back(heap.allocate(1000).value);
    REQUIRE(heap.stats().committed_bytes == committed);
    for (Ref r : big) REQUIRE(heap.free(r) == SlabStatus::Ok);
}

static void forged_and_interior_refs() {
    ActorSlabHeap heap(config(1 << 20, 1 << 20));
    auto a = heap.allocate(32).value;
    REQUIRE(heap.free(forge(a.storage() + 1)) == SlabStatus::InvalidRef); // interior (+8 B)
    REQUIRE(heap.free(forge(0)) == SlabStatus::InvalidRef);               // null
    REQUIRE(heap.free(forge(1)) == SlabStatus::InvalidRef);               // page header
    REQUIRE(heap.free(forge(0xFFFFFFFFu)) == SlabStatus::InvalidRef);     // beyond reserve
    REQUIRE(heap.free(forge(a.storage() + 512)) == SlabStatus::InvalidRef); // never carved

    // A large object's tail pages are payload. Fill them with bytes that look
    // like page headers; refs into them must still be rejected.
    auto big = heap.allocate(3 * 4096).value;
    auto bp = heap.resolve(big, 3 * 4096);
    REQUIRE(bp);
    std::memset(bp.value, 0x01, 3 * 4096);
    const std::uint32_t tail_word = big.storage() + 4096 / 8; // next page + 64
    REQUIRE(heap.free(forge(tail_word)) == SlabStatus::InvalidRef);
    REQUIRE(heap.resolve(forge(tail_word), 1).status == SlabStatus::InvalidRef);
    REQUIRE(heap.free(big) == SlabStatus::Ok);
    REQUIRE(heap.free(a) == SlabStatus::Ok);
}

static void budget_is_failure_atomic() {
    ActorSlabHeap heap(config(1 << 20, 64 * 1024));
    std::vector<Ref> refs;
    SlabStatus last = SlabStatus::Ok;
    for (;;) {
        auto r = heap.allocate(512);
        if (!r) { last = r.status; break; }
        refs.push_back(r.value);
    }
    REQUIRE(last == SlabStatus::BudgetExceeded);
    REQUIRE(heap.stats().committed_bytes == 64 * 1024);
    const auto before = heap.stats();
    REQUIRE(heap.allocate(512).status == SlabStatus::BudgetExceeded);
    REQUIRE(heap.allocate(100000).status == SlabStatus::BudgetExceeded);
    REQUIRE(heap.stats().live_objects == before.live_objects);
    REQUIRE(heap.stats().live_pages == before.live_pages);
    // Every surviving object is intact and the heap keeps working.
    for (Ref r : refs) REQUIRE(heap.resolve(r, 512));
    REQUIRE(heap.free(refs.back()) == SlabStatus::Ok);
    REQUIRE(heap.allocate(512));
}

static void handles_fifo_stale_and_quarantine() {
    ActorSlabHeap heap(config(1 << 20, 1 << 20, 4));
    REQUIRE(heap.ok());
    REQUIRE(heap.handle_slot_bits() == 3);           // capacity-sized, not a fixed 20/12
    REQUIRE(heap.handle_max_generation() == (1u << 29) - 1);
    auto slot = [&](ActorSlabHeap::Handle32 h) { return h.word & 7u; };
    auto a = heap.allocate_handle(40).value;
    auto b = heap.allocate_handle(40).value;
    REQUIRE(slot(a) == 1 && slot(b) == 2);
    REQUIRE(heap.free_handle(a) == SlabStatus::Ok);
    REQUIRE(heap.resolve_handle(a).status == SlabStatus::StaleHandle);
    REQUIRE(heap.free_handle(a) == SlabStatus::StaleHandle);
    // FIFO: the freed slot goes to the back, behind slots 3 and 4.
    auto c = heap.allocate_handle(40).value;
    auto d = heap.allocate_handle(40).value;
    auto e = heap.allocate_handle(40).value;
    REQUIRE(slot(c) == 3 && slot(d) == 4 && slot(e) == 1);
    REQUIRE(!(e == a)); // same slot, new generation
    REQUIRE(heap.allocate_handle(40).status == SlabStatus::HandleTableFull);
    for (auto h : {b, c, d, e}) REQUIRE(heap.free_handle(h) == SlabStatus::Ok);

    // Generation exhaustion: 2 generation bits = generations 1..3, then quarantine.
    ActorSlabHeap small(config(1 << 20, 1 << 20, 2, 2));
    REQUIRE(small.ok() && small.handle_max_generation() == 3);
    int allocations = 0;
    for (;;) {
        auto h = small.allocate_handle(16);
        if (!h) { REQUIRE(h.status == SlabStatus::HandleTableFull); break; }
        ++allocations;
        REQUIRE(small.free_handle(h.value) == SlabStatus::Ok);
    }
    REQUIRE(allocations == 2 * 3);                 // every slot used every generation
    REQUIRE(small.stats().quarantined_handles == 2);
    REQUIRE(small.allocate(16));                   // Offset32 path unaffected
    ActorSlabHeap bad(config(1 << 20, 1 << 20, 4, 30)); // 3 slot bits + 30 > 32
    REQUIRE(!bad.ok());
}

static void stress_fragmentation() {
    ActorSlabHeap heap(config(8 << 20, 8 << 20));
    struct Live { Ref ref; std::uint32_t bytes; std::uint8_t tag; };
    std::vector<Live> live;
    std::uint32_t lcg = 12345;
    auto next = [&] { lcg = lcg * 1664525u + 1013904223u; return lcg >> 8; };
    for (int op = 0; op < 200000; ++op) {
        if (live.empty() || next() % 100 < 55) {
            const std::uint32_t bytes = next() % 100 < 95 ? 1 + next() % 256 : 2049 + next() % 9000;
            auto r = heap.allocate(bytes);
            if (!r) { REQUIRE(r.status == SlabStatus::BudgetExceeded || r.status == SlabStatus::OutOfMemory); continue; }
            const auto tag = static_cast<std::uint8_t>(next());
            std::memset(heap.resolve(r.value, bytes).value, tag, bytes);
            live.push_back({r.value, bytes, tag});
        } else {
            const std::size_t i = next() % live.size();
            auto* bytes = static_cast<std::uint8_t*>(heap.resolve(live[i].ref, live[i].bytes).value);
            REQUIRE(bytes != nullptr);
            for (std::uint32_t k = 0; k < live[i].bytes; ++k) REQUIRE(bytes[k] == live[i].tag);
            REQUIRE(heap.free(live[i].ref) == SlabStatus::Ok);
            live[i] = live.back();
            live.pop_back();
        }
    }
    for (auto& l : live) REQUIRE(heap.free(l.ref) == SlabStatus::Ok);
    REQUIRE(heap.stats().live_objects == 0);
    REQUIRE(heap.stats().live_pages == 0);
}

static void caller_buffer_and_retire() {
    alignas(4096) static std::byte buffer[16 * 4096];
    ActorSlabHeap heap(buffer, sizeof(buffer), 8);
    REQUIRE(heap.ok());
    auto h = heap.allocate_handle(64).value;
    REQUIRE(heap.resolve_handle(h));
    ActorSlabHeap misaligned(buffer + 8, 4096 * 4);
    REQUIRE(!misaligned.ok());
    heap.retire();
    REQUIRE(heap.allocate(16).status == SlabStatus::Retired);
    REQUIRE(heap.resolve_handle(h).status == SlabStatus::Retired);
    for (std::byte b : buffer) REQUIRE(b == std::byte{0}); // scrubbed on STOP
    ActorSlabHeap mapped(config(1 << 20, 1 << 20));
    REQUIRE(mapped.allocate(100));
    mapped.trim();
    mapped.retire();
    REQUIRE(mapped.stats().committed_bytes == 0);
    REQUIRE(mapped.free(forge(1)) == SlabStatus::Retired);
}

static void rejects_bad_config() {
    REQUIRE(!ActorSlabHeap(config(0, 0)).ok());
    REQUIRE(!ActorSlabHeap(config(1 << 20, 2 << 20)).ok()); // budget > reserve
}

int main() {
    basic_and_classes();
    page_packing_and_reuse();
    forged_and_interior_refs();
    budget_is_failure_atomic();
    handles_fifo_stale_and_quarantine();
    stress_fragmentation();
    caller_buffer_and_retire();
    rejects_bad_config();
    std::printf("PASS actor slab heap: %d checks, %zu-bit host, 4-byte Offset32/Handle32\n",
                checks, sizeof(void*) * 8);
    return 0;
}
