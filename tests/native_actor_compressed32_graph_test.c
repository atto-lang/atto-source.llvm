#include "oreslang/runtime/ActorArena.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#if defined(__unix__) || defined(__APPLE__)
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "actor graph check failed line %d\n", __LINE__); exit(1); } } while (0)
_Static_assert(sizeof(uint32_t) == 4, "graph edge must use exactly four bytes");

static int round_trip(void) {
    OresActorArena* a = ores_actor_arena_open(501, 1, 3, 64, 64);
    OresActorArena* b = ores_actor_arena_open(502, 1, 3, 64, 64);
    OresArenaTurn at = ores_actor_arena_enter(a, 501, 1);
    OresArenaHandle ah = ores_actor_arena_alloc(a, at, 16);
    OresArenaHandle av = ores_actor_arena_alloc(a, at, 16);
    const uint32_t a_root = ores_actor_arena_compact_publish_i32(a, at.domain, at.epoch, ah.index, ah.generation);
    const uint32_t a_leaf = ores_actor_arena_compact_publish_i32(a, at.domain, at.epoch, av.index, av.generation);
    CHECK(a_root == 1 && a_leaf == 9);
    ores_actor_arena_compact_store_i64(a, at.domain, at.epoch, a_leaf, 8, UINT64_C(0xabcdef1234567890));
    ores_actor_arena_compact_store_ref_i32(a, at.domain, at.epoch, a_root, 4, a_leaf);
    /* Field bytes are canonical little-endian, even on a big-endian host.
     * The host-facing scalar ABI and opaque actor descriptors are separate. */
    unsigned char field_bytes[sizeof(uint32_t)] = {0, 0, 0, 0};
    ores_actor_arena_read(a, at, ah, 4, field_bytes, sizeof(field_bytes));
    for (unsigned i = 0; i < sizeof(field_bytes); ++i)
        CHECK(field_bytes[i] == (unsigned char) (a_leaf >> (8u * i)));
    CHECK(ores_actor_arena_compact_load_ref_i32(a, at.domain, at.epoch, a_root, 4) == a_leaf);
    uint32_t followed = ores_actor_arena_compact_load_ref_i32(a, at.domain, at.epoch, a_root, 4);
    CHECK(ores_actor_arena_compact_load_i64(a, at.domain, at.epoch, followed, 8) == UINT64_C(0xabcdef1234567890));
    ores_actor_arena_compact_store_ref_i32(a, at.domain, at.epoch, a_root, 4, 0);
    ores_actor_arena_read(a, at, ah, 4, field_bytes, sizeof(field_bytes));
    for (unsigned i = 0; i < sizeof(field_bytes); ++i)
        CHECK(field_bytes[i] == 0);
    CHECK(ores_actor_arena_compact_load_ref_i32(a, at.domain, at.epoch, a_root, 4) == 0);
    ores_actor_arena_compact_store_ref_i32(a, at.domain, at.epoch, a_root, 4, a_leaf);
    ores_actor_arena_leave(a, at);

    OresArenaTurn bt = ores_actor_arena_enter(b, 502, 1);
    OresArenaHandle bh = ores_actor_arena_alloc(b, bt, 16);
    OresArenaHandle bv = ores_actor_arena_alloc(b, bt, 16);
    const uint32_t b_root = ores_actor_arena_compact_publish_i32(b, bt.domain, bt.epoch, bh.index, bh.generation);
    const uint32_t b_leaf = ores_actor_arena_compact_publish_i32(b, bt.domain, bt.epoch, bv.index, bv.generation);
    CHECK(a_root == b_root && a_leaf == b_leaf && at.domain != bt.domain);
    ores_actor_arena_compact_store_i64(b, bt.domain, bt.epoch, b_leaf, 8, UINT64_C(0x9876543210fedcba));
    ores_actor_arena_compact_store_ref_i32(b, bt.domain, bt.epoch, b_root, 4, b_leaf);
    CHECK(ores_actor_arena_compact_load_i64(b, bt.domain, bt.epoch,
        ores_actor_arena_compact_load_ref_i32(b, bt.domain, bt.epoch, b_root, 4), 8) ==
        UINT64_C(0x9876543210fedcba));
    ores_actor_arena_leave(b, bt);

    at = ores_actor_arena_enter(a, 501, 1);
    CHECK(ores_actor_arena_compact_load_ref_i32(a, at.domain, at.epoch, a_root, 4) == a_leaf);
    ores_actor_arena_drop(a, at, av);
    // A stale edge must not resolve to a new object: published slots never recycle.
    OresArenaHandle replacement = ores_actor_arena_alloc(a, at, 16);
    CHECK(replacement.index != av.index);
    ores_actor_arena_leave(a, at);
    ores_actor_arena_retire(a);
    ores_actor_arena_retire(b);
    CHECK(ores_actor_arena_try_destroy(a) == 1);
    CHECK(ores_actor_arena_try_destroy(b) == 1);
    return 0;
}

/* Encode a reference whose second byte is nonzero. Testing only slot 1
 * (word 9) misses byte-order regressions outside its low byte. */
static int multi_byte_compact_ref_wire_format(void) {
    OresActorArena* a = ores_actor_arena_open(704, 1, 40, 64, 64);
    OresArenaTurn t = ores_actor_arena_enter(a, 704, 1);
    OresArenaHandle owner = ores_actor_arena_alloc(a, t, 16);
    const uint32_t source_bits = ores_actor_arena_compact_publish_i32(
        a, t.domain, t.epoch, owner.index, owner.generation);
    uint32_t target_bits = 0;
    for (unsigned i = 0; i < 32; ++i) {
        OresArenaHandle h = ores_actor_arena_alloc(a, t, 16);
        if (i == 31) {
            target_bits = ores_actor_arena_compact_publish_i32(
                a, t.domain, t.epoch, h.index, h.generation);
        }
    }
    CHECK(source_bits == 1);
    CHECK(target_bits == 257); /* slot index 32, stride 64, shift 3 */
    ores_actor_arena_compact_store_ref_i32(
        a, t.domain, t.epoch, source_bits, 4, target_bits);
    unsigned char wire[4] = {0, 0, 0, 0};
    ores_actor_arena_read(a, t, owner, 4, wire, sizeof(wire));
    CHECK(wire[0] == 1 && wire[1] == 1 && wire[2] == 0 && wire[3] == 0);
    CHECK(ores_actor_arena_compact_load_ref_i32(
        a, t.domain, t.epoch, source_bits, 4) == target_bits);
    ores_actor_arena_leave(a, t);
    ores_actor_arena_retire(a);
    CHECK(ores_actor_arena_try_destroy(a) == 1);
    return 0;
}

enum violation {
    FORGED_INTERIOR, UNPUBLISHED_TARGET, DANGLING_EDGE,
    BORROWED_SOURCE, MISALIGNED_FIELD, FIELD_OVERFLOW,
    INVALID_SOURCE, FORGED_DOMAIN, CORRUPT_STORED_BITS,
    INVALID_SOURCE_TURN, INVALID_TARGET_ON_STORE
};
static void invalid(enum violation reason) {
    OresActorArena* a = ores_actor_arena_open(603, 1, 3, 64, 64);
    OresArenaTurn t = ores_actor_arena_enter(a, 603, 1);
    OresArenaHandle h = ores_actor_arena_alloc(a, t, 16);
    const uint32_t root = ores_actor_arena_compact_publish_i32(a, t.domain, t.epoch, h.index, h.generation);
    uint32_t leaf = 0;
    OresArenaHandle child = {0, 0, 0};
    if (reason == UNPUBLISHED_TARGET || reason == DANGLING_EDGE ||
        reason == INVALID_TARGET_ON_STORE) {
        child = ores_actor_arena_alloc(a, t, 16);
        leaf = root + 8;
        if (reason != UNPUBLISHED_TARGET)
            leaf = ores_actor_arena_compact_publish_i32(
                a, t.domain, t.epoch, child.index, child.generation);
    }
    switch (reason) {
    case FORGED_INTERIOR:
        ores_actor_arena_compact_store_ref_i32(a, t.domain, t.epoch, root, 0, root + 1); break;
    case UNPUBLISHED_TARGET:
        ores_actor_arena_compact_store_ref_i32(a, t.domain, t.epoch, root, 0, leaf); break;
    case DANGLING_EDGE:
        ores_actor_arena_compact_store_ref_i32(a, t.domain, t.epoch, root, 0, leaf);
        ores_actor_arena_drop(a, t, child);
        (void) ores_actor_arena_compact_load_ref_i32(a, t.domain, t.epoch, root, 0); break;
    case BORROWED_SOURCE: {
        OresArenaLoan loan = ores_actor_arena_borrow(a, t, h);
        (void)loan;
        ores_actor_arena_compact_store_ref_i32(a, t.domain, t.epoch, root, 0, 0); break;
    }
    case MISALIGNED_FIELD:
        ores_actor_arena_compact_store_ref_i32(a, t.domain, t.epoch, root, 1, 0); break;
    case FIELD_OVERFLOW:
        ores_actor_arena_compact_store_ref_i32(a, t.domain, t.epoch, root, 16, 0); break;
    case INVALID_SOURCE:
        ores_actor_arena_compact_store_ref_i32(a, t.domain, t.epoch, 0, 0, root); break;
    case FORGED_DOMAIN:
        (void) ores_actor_arena_compact_load_ref_i32(a, t.domain + 1, t.epoch, root, 0); break;
    case CORRUPT_STORED_BITS: {
        uint32_t fake = root + 1;
        ores_actor_arena_write(a, t, h, 0, &fake, sizeof(fake));
        (void) ores_actor_arena_compact_load_ref_i32(a, t.domain, t.epoch, root, 0); break;
    }
    case INVALID_SOURCE_TURN:
        (void) ores_actor_arena_compact_load_ref_i32(a, t.domain, t.epoch + 1, root, 0); break;
    case INVALID_TARGET_ON_STORE:
        ores_actor_arena_drop(a, t, child);
        ores_actor_arena_compact_store_ref_i32(a, t.domain, t.epoch, root, 0, leaf); break;
    }
}
#if defined(__unix__) || defined(__APPLE__)
static int must_abort(enum violation reason) {
    pid_t pid = fork();
    if (pid < 0) return 0;
    if (pid == 0) {
        struct rlimit core = {0, 0};
        (void)setrlimit(RLIMIT_CORE, &core);
        invalid(reason);
        _exit(94);
    }
    int status = 0;
    return waitpid(pid, &status, 0) == pid && WIFSIGNALED(status) &&
           WTERMSIG(status) == SIGABRT;
}
#endif
int main(void) {
    CHECK(round_trip() == 0);
    CHECK(multi_byte_compact_ref_wire_format() == 0);
#if defined(__unix__) || defined(__APPLE__)
    for (int n = FORGED_INTERIOR; n <= INVALID_TARGET_ON_STORE; ++n)
        CHECK(must_abort((enum violation)n));
#endif
    puts("ACTOR_COMPRESSED32_GRAPH_PASS");
    return 0;
}
