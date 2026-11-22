#include "oreslang/runtime/ActorArena.h"
#include <stdint.h>
#include <stdio.h>

int main(void) {
    OresActorArena* arena = ores_actor_arena_open(7u, 1u, 8u, 128u, 4096u);
    if (!arena) return 1;
    if (ores_actor_arena_is_prefaulted(arena)) return 2;
    if (!ores_actor_arena_prefault(arena)) return 3;
    if (!ores_actor_arena_is_prefaulted(arena)) return 4;
    OresArenaTurn turn = ores_actor_arena_enter_realtime(arena, 7u, 1u);
    OresArenaHandle handle = ores_actor_arena_alloc(arena, turn, sizeof(uint64_t));
    uint64_t value = 42u, observed = 0u;
    ores_actor_arena_write(arena, turn, handle, 0u, &value, sizeof(value));
    ores_actor_arena_read(arena, turn, handle, 0u, &observed, sizeof(observed));
    if (observed != value) return 5;
    /* Prefault is startup-only; it must not run during an admitted turn. */
    if (ores_actor_arena_prefault(arena)) return 6;
    ores_actor_arena_drop(arena, turn, handle);
    ores_actor_arena_leave(arena, turn);
    ores_actor_arena_retire(arena);
    if (!ores_actor_arena_try_destroy(arena)) return 7;
    puts("PASS actor arena startup prefault and deterministic turn allocation");
    return 0;
}
