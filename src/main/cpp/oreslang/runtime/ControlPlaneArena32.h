#ifndef ATTO_CONTROL_PLANE_ARENA32_H
#define ATTO_CONTROL_PLANE_ARENA32_H
#include <stdint.h>

/* HOST-only bounded control plane, never an Atto actor reference or wire ABI. */
#define ATTO_CONTROL_REF32_ABI "control_generation12_slot20_v1"
#define ATTO_CONTROL_RECORD_BYTES 32u
typedef struct AttoControlPlaneArena32 AttoControlPlaneArena32;

AttoControlPlaneArena32 *atto_control32_new(uint32_t capacity);
int atto_control32_allocate(AttoControlPlaneArena32 *arena, uint64_t *out_witness);
int atto_control32_drop(AttoControlPlaneArena32 *arena, uint64_t witness);
int atto_control32_write_i64(AttoControlPlaneArena32 *arena, uint64_t witness, unsigned field, int64_t value);
int atto_control32_read_i64(AttoControlPlaneArena32 *arena, uint64_t witness, unsigned field, int64_t *out);
int atto_control32_write_ref(AttoControlPlaneArena32 *arena, uint64_t owner, uint64_t child);
int atto_control32_read_ref(AttoControlPlaneArena32 *arena, uint64_t owner, uint64_t *out);
void atto_control32_retire(AttoControlPlaneArena32 *arena);
/* Destruction requires external quiescence: no outstanding callers or pins. */
void atto_control32_destroy(AttoControlPlaneArena32 *arena);
#endif
