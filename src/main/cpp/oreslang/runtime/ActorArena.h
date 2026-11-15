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
/* Trusted actor-turn preflight, without mutation or raw pointer export.
 * Returns 1 only if every page belongs to THIS arena, is live at the exact
 * generation, has the requested length, and names a distinct physical slot.
 * Invalid page vectors return 0 before callers touch any backing words.
 * The caller must already hold a valid physical arena turn; misuse of turn
 * authority retains the existing fail-stop rule. This is not an exclusive
 * reservation against separately created host views. */
int ores_actor_arena_validate_pages(OresActorArena*, OresArenaTurn,
                                    const OresArenaHandle* pages,
                                    size_t count, size_t page_bytes);
/* Atomic all-or-none trusted-host page claim. Nonzero tokens are unique
 * within one arena lifetime and never wrap; zero rejects invalid, borrowed,
 * aliased or already-claimed pages without changing their ownership state.
 * This fences competing logical slabs, not every trusted raw arena reader.
 * The caller must hold the currently authenticated physical arena turn. */
uint64_t ores_actor_arena_claim_pages(OresActorArena*, OresArenaTurn,
                                      const OresArenaHandle* pages,
                                      size_t count, size_t page_bytes);
/* Exactly-once release of an entire claim, during its owning arena turn.
 * Return zero for stale/mismatched receipts without altering any claim.
 * The caller MUST first quiesce logical request receipts and scrub all pages. */
int ores_actor_arena_unclaim_pages(OresActorArena*, OresArenaTurn,
                                   const OresArenaHandle* pages,
                                   size_t count, uint64_t claim);
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
