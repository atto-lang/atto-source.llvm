/* Exact native ActorArena.c lifecycle stress: host substrate, NOT .atto guest.
 * Reuse bounded slots over many turns, require scratch zeroing, preserve a
 * persistent actor-owned slot, reject cross-domain and stale handles, and
 * pin physical destruction while a cross-turn loan is outstanding.
 */
#include "oreslang/runtime/ActorArena.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#define CYCLES 4096u
/* These linker wrappers intercept direct allocator references from the
 * production ActorArena.c object. They do NOT cover arbitrary libc internals,
 * foreign modules or the full AttoLang compiled guest runtime. */
static volatile sig_atomic_t admitted_actor_turn;
static size_t startup_callocs;
static size_t physical_frees;
void* __real_calloc(size_t, size_t);
void* __real_malloc(size_t);
void* __real_realloc(void*, size_t);
void __real_free(void*);
void* __wrap_calloc(size_t count, size_t bytes) {
    if (admitted_actor_turn) abort();
    void* p = __real_calloc(count, bytes);
    if (p) ++startup_callocs;
    return p;
}
void* __wrap_malloc(size_t bytes) {
    if (admitted_actor_turn) abort();
    return __real_malloc(bytes);
}
void* __wrap_realloc(void* memory, size_t bytes) {
    if (admitted_actor_turn) abort();
    return __real_realloc(memory, bytes);
}
void __wrap_free(void* memory) {
    if (admitted_actor_turn) abort();
    if (memory) ++physical_frees;
    __real_free(memory);
}
#define CHECK(ok, why) do { if (!(ok)) { \
    fprintf(stderr, "ACTOR_ARENA_LIFECYCLE_FAIL: %s at line %d\n", why, __LINE__); \
    return 1; \
} } while (0)

typedef struct {
    OresActorArena* arena;
    uint64_t owner, incarnation, marker;
    OresArenaHandle anchor;
    OresArenaHandle last_dropped;
} Actor;

static int start_actor(Actor* actor, uint64_t owner, uint64_t marker) {
    memset(actor, 0, sizeof(*actor));
    actor->owner = owner;
    actor->incarnation = 1u;
    actor->marker = marker;
    actor->arena = ores_actor_arena_open(owner, actor->incarnation, 4u, 64u, 256u);
    CHECK(actor->arena != NULL, "actor arena creation");
    CHECK(!ores_actor_arena_is_prefaulted(actor->arena), "unexpected prefault");
    CHECK(ores_actor_arena_prefault(actor->arena), "startup prefault");
    admitted_actor_turn = 1;
    OresArenaTurn turn = ores_actor_arena_enter_realtime(actor->arena, owner, 1u);
    actor->anchor = ores_actor_arena_alloc(actor->arena, turn, sizeof(marker));
    ores_actor_arena_write(actor->arena, turn, actor->anchor, 0u, &marker, sizeof(marker));
    ores_actor_arena_leave(actor->arena, turn);
    admitted_actor_turn = 0;
    return 0;
}

static int actor_turn(Actor* actor, uint64_t iteration) {
    admitted_actor_turn = 1;
    OresArenaTurn turn = ores_actor_arena_enter_realtime(
        actor->arena, actor->owner, actor->incarnation);
    uint64_t observed = 0;
    ores_actor_arena_read(actor->arena, turn, actor->anchor, 0u,
                          &observed, sizeof(observed));
    CHECK(observed == actor->marker, "persistent actor state corrupted");
    uint64_t values[2] = {actor->marker, iteration};
    uint64_t out[2] = {0, 0};
    OresArenaHandle temporary = ores_actor_arena_alloc(
        actor->arena, turn, sizeof(values));
    ores_actor_arena_write(actor->arena, turn, temporary, 0u, values, sizeof(values));
    ores_actor_arena_read(actor->arena, turn, temporary, 0u, out, sizeof(out));
    CHECK(out[0] == values[0] && out[1] == values[1], "temporary slot corrupted");
    unsigned char bytes[32] = {0};
    OresArenaScratch scratch = ores_actor_arena_scratch_alloc(
        actor->arena, turn, sizeof(bytes), 8u);
    ores_actor_arena_scratch_read(actor->arena, turn, scratch, 0u,
                                  bytes, sizeof(bytes));
    for (size_t i = 0; i < sizeof(bytes); ++i)
        CHECK(bytes[i] == 0, "scratch not scrubbed after previous turn");
    memset(bytes, 0xA5, sizeof(bytes));
    ores_actor_arena_scratch_write(actor->arena, turn, scratch, 0u,
                                   bytes, sizeof(bytes));
    actor->last_dropped = temporary;
    ores_actor_arena_drop(actor->arena, turn, temporary);
    ores_actor_arena_leave(actor->arena, turn);
    admitted_actor_turn = 0;
    return 0;
}

/* The child must die by the runtime's fail-stop gate, not return or SIGSEGV.
 * Executing negative cases in children keeps the healthy actors alive. */
static int rejects_handle(Actor* actor, OresArenaHandle invalid) {
    const pid_t child = fork();
    if (child < 0) return 0;
    if (child == 0) {
        const struct rlimit no_core = {0u, 0u};
        (void)setrlimit(RLIMIT_CORE, &no_core);
        OresArenaTurn turn = ores_actor_arena_enter_realtime(
            actor->arena, actor->owner, actor->incarnation);
        uint64_t value = 0;
        ores_actor_arena_read(actor->arena, turn, invalid, 0u, &value, sizeof(value));
        _exit(91);
    }
    int status = 0;
    return waitpid(child, &status, 0) == child &&
           WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
}

int main(void) {
    Actor first, second;
    CHECK(!start_actor(&first, 1701u, 0x11223344u), "first actor startup");
    CHECK(!start_actor(&second, 1702u, 0x55667788u), "second actor startup");
    CHECK(first.anchor.domain != second.anchor.domain, "actor domain collision");
    for (uint64_t i = 0; i < CYCLES; ++i) {
        CHECK(!actor_turn(&first, i), "first turn failed");
        CHECK(!actor_turn(&second, i), "second turn failed");
    }
    CHECK(rejects_handle(&first, second.anchor), "foreign actor handle admitted");
    CHECK(rejects_handle(&first, first.last_dropped), "stale generation admitted");
    admitted_actor_turn = 1;
    OresArenaTurn turn = ores_actor_arena_enter_realtime(
        first.arena, first.owner, first.incarnation);
    OresArenaLoan loan = ores_actor_arena_borrow(first.arena, turn, first.anchor);
    ores_actor_arena_leave(first.arena, turn);
    admitted_actor_turn = 0;
    ores_actor_arena_retire(first.arena);
    CHECK(!ores_actor_arena_try_destroy(first.arena),
          "retirement freed arena while loan was outstanding");
    ores_actor_arena_release(first.arena, loan);
    CHECK(ores_actor_arena_try_destroy(first.arena), "arena not reclaimed after loan release");
    CHECK(!actor_turn(&second, CYCLES), "sibling actor corrupted by retirement");
    ores_actor_arena_retire(second.arena);
    CHECK(ores_actor_arena_try_destroy(second.arena), "second arena not reclaimed");
    CHECK(startup_callocs == 10u, "native arena startup allocation count");
    CHECK(physical_frees == 10u, "unbalanced actor physical reclamation");
    puts("LLVM_ACTOR_ARENA_HOST_LIFECYCLE_PASS|parent_turns=8196|stress_turns=8193|actors=2|negative_children=2|startup_callocs=10|physical_frees=10|turn_allocations=0|native_guest=NO|zero_gc_certified=NO");
    return 0;
}
