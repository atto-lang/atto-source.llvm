/* Link with -Wl,--wrap=calloc,--wrap=free against the real ActorArena.c.
 * Observe bytes immediately before the real free, never after their lifetime.
 */
#include "oreslang/runtime/ActorArena.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

void* __real_calloc(size_t, size_t);
void __real_free(void*);
static struct { void* pointer; size_t bytes; } allocations[16];
static size_t live;
void* __wrap_calloc(size_t count, size_t bytes) {
    void* pointer = __real_calloc(count, bytes);
    if (!pointer || live == 16) abort();
    allocations[live].pointer = pointer;
    allocations[live++].bytes = count * bytes;
    return pointer;
}
void __wrap_free(void* pointer) {
    if (!pointer) return;
    size_t index = 0;
    while (index < live && allocations[index].pointer != pointer) ++index;
    if (index == live) abort();
    const volatile unsigned char* p = pointer;
    for (size_t i = 0; i < allocations[index].bytes; ++i) {
        if (p[i] != 0) {
            fputs("unscrubbed actor arena bytes reached free\n", stderr);
            exit(1);
        }
    }
    allocations[index] = allocations[--live];
    __real_free(pointer);
}
int main(void) {
    OresActorArena* arena = ores_actor_arena_open(61, 7, 2, 64, 128);
    OresArenaTurn turn = ores_actor_arena_enter(arena, 61, 7);
    OresArenaHandle handle = ores_actor_arena_alloc(arena, turn, 64);
    unsigned char secret[64];
    for (size_t i = 0; i < sizeof(secret); ++i) secret[i] = 0xa7;
    ores_actor_arena_write(arena, turn, handle, 0, secret, sizeof(secret));
    OresArenaScratch scratch = ores_actor_arena_scratch_alloc(arena, turn, 64, 8);
    ores_actor_arena_scratch_write(arena, turn, scratch, 0, secret, sizeof(secret));
    ores_actor_arena_leave(arena, turn);
    ores_actor_arena_retire(arena);
    if (!ores_actor_arena_try_destroy(arena) || live != 0) return 1;
    puts("PASS actor arena backing and metadata scrubbed before free");
    return 0;
}
