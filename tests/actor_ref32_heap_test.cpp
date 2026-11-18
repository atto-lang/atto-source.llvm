// Tests the experimental HOST-side Ref32 adapter against the real M2b slab.
// Not a compiled .atto guest, guest ABI v5 or a native sandbox proof.
#include "oreslang/runtime/ActorRef32Heap.hpp"
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <type_traits>
#include <vector>

using namespace oreslang::runtime::vm;
static int checks = 0;
static void check(bool v, const char* what) {
    ++checks;
    if (!v) throw std::runtime_error(what);
}
int main() {
    const ActorHeapOwner owner{7, 9, 1}, other{7, 10, 1}, restart{7, 9, 2};
    std::vector<std::uint8_t> cage(16384, 0);
    ActorRef32Heap a(owner, cage.data(), 1024, 4096, 2);
    ActorRef32Heap b(other, cage.data(), 8192, 2048, 2);
    check(a.sidecar_bytes() > 0, "missing trusted sidecar");
    static_assert(!std::is_constructible_v<TrustedActorRef32, std::uint32_t, ActorHeapOwner>,
                  "guest must not fabricate host origin witnesses");
    check(sizeof(std::uint32_t) == 4, "guest token width");
    auto x = a.allocate(owner, 24, 8, 101);  // Point {i64 x,y} + velocity i64
    auto y = b.allocate(other, 24, 8, 101);
    check(x.guest_token() != 0 && x.guest_token() == y.guest_token(), "sibling numeric collision test");
    check(a.write_i64(x, owner, 101, 0, 10), "write point x");
    check(a.write_i64(x, owner, 101, 8, 20), "write point y");
    check(a.write_i64(x, owner, 101, 16, 3), "write velocity");
    std::int64_t v = -1;
    check(a.read_i64(x, owner, 101, 0, v) && v == 10, "read x");
    check(a.read_i64(x, owner, 101, 8, v) && v == 20, "read y");
    check(a.read_i64(x, owner, 101, 16, v) && v == 3, "read velocity");
    check(!a.read_i64(x, owner, 101, 24, v), "OOB read");
    check(!a.write_i64(x, owner, 101, 1, 2), "unaligned field");
    check(!a.write_i64(x, owner, 99, 0, 2), "type forgery");
    check(!a.read_i64(x, other, 101, 0, v), "foreign caller context");
    check(!a.read_i64(x, restart, 101, 0, v), "incarnation replay");
    check(!a.read_i64(y, owner, 101, 0, v), "foreign handle numeric collision");
    check(!b.read_i64(x, other, 101, 0, v), "opposite foreign handle collision");
    check(!a.release(y, owner, 101), "foreign release");
    check(!a.release(x, owner, 999), "wrong-type release");
    check(a.release(x, owner, 101), "release owned object");
    check(!a.release(x, owner, 101), "double-free rejected");
    const auto z = a.allocate(owner, 24, 8, 101);
    check(z.guest_token() && z.guest_token() != x.guest_token(), "generation advanced");
    check(!a.read_i64(x, owner, 101, 0, v), "stale ABA rejected");
    check(a.read_i64(z, owner, 101, 0, v) && v == 0, "freed storage scrubbed");
    check(a.allocate(owner, 24, 8, 0).guest_token() == 0, "type zero accepted");
    check(a.allocate(owner, 0, 8, 101).guest_token() == 0, "zero-size accepted");
    check(a.allocate(other, 24, 8, 101).guest_token() == 0, "foreign allocation");
    check(a.close_after_quiescence() && a.close_after_quiescence(), "idempotent STOP");
    check(a.closed(), "closed state");
    check(!a.read_i64(z, owner, 101, 0, v), "post-STOP read");
    check(a.allocate(owner, 24, 8, 101).guest_token() == 0, "post-STOP alloc");
    check(b.read_i64(y, other, 101, 0, v) && v == 0, "sibling survived STOP");
    check(b.close_after_quiescence(), "sibling close");
    // Exhaust the finite generation bits: never wrap into an old token.
    std::vector<std::uint8_t> little(8192, 0);
    ActorRef32Heap wraps(owner, little.data(), 1024, 2048, 1);
    TrustedActorRef32 previous{};
    for (std::uint32_t generation = 1; generation <= ActorRef32Heap::kMaxGeneration;
         ++generation) {
        const auto now = wraps.allocate(owner, 8, 8, 15);
        check(now.guest_token() != 0 && (now.guest_token() >> ActorRef32Heap::kSlotBits) == generation,
              "generation sequence");
        check(!previous.guest_token() || !wraps.read_i64(previous, owner, 15, 0, v),
              "earlier handle unexpectedly valid");
        check(wraps.release(now, owner, 15), "generation-cycle release");
        previous = now;
    }
    check(wraps.allocate(owner, 8, 8, 15).guest_token() == 0, "generation-wrap slot reused");
    check(!wraps.read_i64(previous, owner, 15, 0, v), "retired token revived");
    bool rejected = false;
    try { ActorRef32Heap invalid(owner, little.data(), 1024, 1024, 0); }
    catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "zero slot count accepted");
    std::cout << "ATTO_REF32_HOST_PASS checks=" << checks
              << " note=not_compiled_atto_guest\n";
    return 0;
}
