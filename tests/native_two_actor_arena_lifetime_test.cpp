#include "oreslang/runtime/ActorArena.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#define CHECK(test) do { \
    if (!(test)) { \
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #test); \
        std::abort(); \
    } \
} while (false)

// Host-native actor arena test ONLY. This is not compiled AttoLang guest code,
// a scheduler/continuation proof, or a certification of process-wide zero GC.
int main() {
    constexpr std::uint64_t id_a = 101;
    constexpr std::uint64_t id_b = 202;
    constexpr std::uint64_t incarnation_a = 7;
    constexpr std::uint64_t incarnation_b = 11;
    OresActorArena* a = ores_actor_arena_open(id_a, incarnation_a, 2, 64, 128);
    OresActorArena* b = ores_actor_arena_open(id_b, incarnation_b, 2, 64, 128);
    CHECK(a != nullptr && b != nullptr);

    OresArenaIdentity ia{}, ib{};
    CHECK(ores_actor_arena_identity(a, &ia) == 1);
    CHECK(ores_actor_arena_identity(b, &ib) == 1);
    CHECK(ia.domain != ib.domain);
    CHECK(ia.owner == id_a && ib.owner == id_b);
    CHECK(ia.incarnation == incarnation_a && ib.incarnation == incarnation_b);

    const auto ta0 = ores_actor_arena_enter(a, id_a, incarnation_a);
    CHECK(ta0.domain == ia.domain && ta0.epoch != 0);
    const auto ha = ores_actor_arena_alloc(a, ta0, sizeof(std::uint64_t));
    CHECK(ha.domain == ia.domain);
    std::uint64_t ca = 17;
    ores_actor_arena_write(a, ta0, ha, 0, &ca, sizeof(ca));
    const auto scratch = ores_actor_arena_scratch_alloc(
        a, ta0, sizeof(std::uint64_t), alignof(std::uint64_t));
    std::uint64_t transient = 0x7f7f7f7f7f7f7f7fULL;
    ores_actor_arena_scratch_write(a, ta0, scratch, 0,
                                   &transient, sizeof(transient));
    ores_actor_arena_leave(a, ta0);

    const auto tb0 = ores_actor_arena_enter(b, id_b, incarnation_b);
    CHECK(tb0.domain == ib.domain && tb0.domain != ta0.domain);
    const auto hb = ores_actor_arena_alloc(b, tb0, sizeof(std::uint64_t));
    CHECK(hb.domain == ib.domain && hb.domain != ha.domain);
    std::uint64_t cb = 230;
    ores_actor_arena_write(b, tb0, hb, 0, &cb, sizeof(cb));
    ores_actor_arena_leave(b, tb0);

    // The actor-owned persistent values survive distinct turns; scratch is
    // zeroized after leave. No actor sees a raw pointer to the other heap.
    for (std::uint64_t i = 0; i < 1000; ++i) {
        const auto ta = ores_actor_arena_enter(a, id_a, incarnation_a);
        CHECK(ta.domain == ia.domain);
        std::uint64_t observed_a = 0;
        ores_actor_arena_read(a, ta, ha, 0, &observed_a, sizeof(observed_a));
        CHECK(observed_a == ca);
        ++ca;
        ores_actor_arena_write(a, ta, ha, 0, &ca, sizeof(ca));
        if (i == 0) {
            const auto fresh_scratch = ores_actor_arena_scratch_alloc(
                a, ta, sizeof(std::uint64_t), alignof(std::uint64_t));
            std::uint64_t cleared = ~std::uint64_t{0};
            ores_actor_arena_scratch_read(a, ta, fresh_scratch, 0,
                                          &cleared, sizeof(cleared));
            CHECK(cleared == 0);
        }
        ores_actor_arena_leave(a, ta);

        const auto tb = ores_actor_arena_enter(b, id_b, incarnation_b);
        std::uint64_t observed_b = 0;
        ores_actor_arena_read(b, tb, hb, 0, &observed_b, sizeof(observed_b));
        CHECK(observed_b == cb);
        cb += 2;
        ores_actor_arena_write(b, tb, hb, 0, &cb, sizeof(cb));
        ores_actor_arena_leave(b, tb);
    }
    CHECK(ca == 1017 && cb == 2230);

    // A lease retained across turn exit prevents physical destruction.
    const auto ta1 = ores_actor_arena_enter(a, id_a, incarnation_a);
    const auto lease = ores_actor_arena_borrow(a, ta1, ha);
    CHECK(lease.domain == ia.domain && lease.nonce != 0);
    ores_actor_arena_leave(a, ta1);
    ores_actor_arena_retire(a);
    CHECK(ores_actor_arena_try_destroy(a) == 0);

    // Actor B remains independently usable after actor A stops.
    const auto tb1 = ores_actor_arena_enter(b, id_b, incarnation_b);
    std::uint64_t after_a_stopped = 0;
    ores_actor_arena_read(b, tb1, hb, 0, &after_a_stopped,
                          sizeof(after_a_stopped));
    CHECK(after_a_stopped == 2230);
    ores_actor_arena_leave(b, tb1);

    ores_actor_arena_release(a, lease);
    CHECK(ores_actor_arena_try_destroy(a) == 1);
    // A is freed: no stale pointer dereference, no double destroy.
    const auto tb2 = ores_actor_arena_enter(b, id_b, incarnation_b);
    ores_actor_arena_drop(b, tb2, hb);
    ores_actor_arena_leave(b, tb2);
    ores_actor_arena_retire(b);
    CHECK(ores_actor_arena_try_destroy(b) == 1);
    std::puts("native-two-actor-arena-substrate: PASS");
    return 0;
}
