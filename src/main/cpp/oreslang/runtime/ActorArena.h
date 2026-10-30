#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Host-only, bounded native actor arena. No guest-visible raw pointers.
 * Persistent fixed-size slab slots survive mailbox turns; scratch allocations
 * are bump-allocated and invalidated/zeroed on each leave. */
typedef struct OresActorArena OresActorArena;
typedef struct { uint64_t domain, epoch; } OresArenaTurn;
typedef struct { uint64_t domain, owner, incarnation; } OresArenaIdentity;
typedef struct { uint64_t domain, index, generation; } OresArenaHandle;
typedef struct { uint64_t domain, nonce, index, generation; } OresArenaLoan;
typedef struct { uint64_t domain, epoch, offset, length; } OresArenaScratch;
OresActorArena* ores_actor_arena_open(uint64_t owner, uint64_t incarnation,
                                       size_t slots, size_t slot_bytes, size_t scratch_bytes);
/* Startup-only page residency preparation for hard-RT actors. This performs
 * volatile touches across every arena allocation before a turn is admitted.
 * It does not lock pages in RAM; the deployment adapter must additionally
 * provide the OS/RTOS memory-locking guarantee required by its RT profile. */
int ores_actor_arena_prefault(OresActorArena*);
/* True only after successful startup prefault and before destruction. */
int ores_actor_arena_is_prefaulted(OresActorArena*);
/* Read-only opaque identity; exposes no arena pointer or storage address. */
int ores_actor_arena_identity(OresActorArena*, OresArenaIdentity*);
OresArenaTurn ores_actor_arena_enter(OresActorArena*, uint64_t owner, uint64_t incarnation);
/* Hard-RT entry is distinct from ordinary actor entry and fails closed unless
 * startup prefault completed. OS/RTOS page locking remains a deployment gate. */
OresArenaTurn ores_actor_arena_enter_realtime(
        OresActorArena*, uint64_t owner, uint64_t incarnation);
void ores_actor_arena_leave(OresActorArena*, OresArenaTurn);
OresArenaHandle ores_actor_arena_alloc(OresActorArena*, OresArenaTurn, size_t bytes);
void ores_actor_arena_read(OresActorArena*, OresArenaTurn, OresArenaHandle, size_t offset,
                           void* out, size_t bytes);
void ores_actor_arena_write(OresActorArena*, OresArenaTurn, OresArenaHandle, size_t offset,
                            const void* src, size_t bytes);
void ores_actor_arena_drop(OresActorArena*, OresArenaTurn, OresArenaHandle);
OresArenaLoan ores_actor_arena_borrow(OresActorArena*, OresArenaTurn, OresArenaHandle);
void ores_actor_arena_release(OresActorArena*, OresArenaLoan);
OresArenaScratch ores_actor_arena_scratch_alloc(OresActorArena*, OresArenaTurn,
                                                size_t bytes, size_t alignment);
void ores_actor_arena_scratch_write(OresActorArena*, OresArenaTurn, OresArenaScratch,
                                    size_t offset, const void* src, size_t bytes);
void ores_actor_arena_scratch_read(OresActorArena*, OresArenaTurn, OresArenaScratch,
                                   size_t offset, void* out, size_t bytes);
void ores_actor_arena_retire(OresActorArena*);
/* Returns 0 while a turn or persistent loan exists; otherwise zeroizes and
 * frees storage and returns 1. Host must fence external callbacks first. */
int ores_actor_arena_try_destroy(OresActorArena*);
#ifdef __cplusplus
}
#endif
