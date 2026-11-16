#pragma once
#include "NativeExecutionAuthority.h"
#include "NativeExecutionContext.h"
#include <stddef.h>
#include <stdint.h>

/*
 * Host-only bounded f32 GEMM call admission, staged for common-source
 * contracts/v1/oresllm-actor-f32-host-abi-v1.json.
 *
 * This does NOT call the OresLLM tensor broker, mint guest capabilities,
 * register a compiler intrinsic, or certify LLVM AOT execution. A returned
 * PREFLIGHT_ONLY is NOT permission to run a guest-provided C pointer.
 *
 * Scope must be constructed and held by the trusted scheduler, never by a
 * guest, and must not outlive its actor execution context or physical turn.
 */
typedef enum {
    ORES_F32_HOST_INVALID = 1,
    ORES_F32_HOST_WRONG_ACTOR = 2,
    ORES_F32_HOST_NOT_RUNNING = 3,
    ORES_F32_HOST_BAD_SHAPE = 4,
    ORES_F32_HOST_QUOTA = 5,
    ORES_F32_HOST_PREFLIGHT_ONLY = 6
} OresF32HostAdmissionResult;

typedef struct {
    const OresNativeExecutionContext *context;
    OresNativeExecutionAuthority *physical_authority;
    OresExecutionClaim physical_claim;
    uint64_t expected_runtime_domain;
    uint64_t expected_execution;
    uint64_t expected_actor_domain;
    uint64_t expected_actor_owner;
    uint64_t expected_actor_generation;
} OresF32ActorHostScope;

/*
 * For an actual source-compiled guest entry, the runtime must still:
 *  (1) keep the physical turn pinned through the broker call;
 *  (2) map these trusted identities to a broker-owned slot and exact shape;
 *  (3) check generation/quotas again at the broker's atomic admission;
 *  (4) publish output only after success, release on every failure path;
 *  (5) prove compiler-owned borrow and rt take lowering.
 */
static inline OresF32HostAdmissionResult ores_f32_host_preflight_v1(
        const OresF32ActorHostScope *scope,
        size_t lhs_rows, size_t lhs_cols,
        size_t rhs_rows, size_t rhs_cols,
        size_t max_output_bytes) {
    if (!scope || !scope->context || !scope->physical_authority ||
            !scope->expected_runtime_domain || !scope->expected_execution ||
            !scope->expected_actor_domain || !scope->expected_actor_owner ||
            !scope->expected_actor_generation)
        return ORES_F32_HOST_INVALID;
    const OresNativeExecutionContext *ctx = scope->context;
    if (ctx->allocation.kind != ORES_ALLOCATION_DOMAIN_ACTOR ||
            ctx->id.runtime_domain != scope->expected_runtime_domain ||
            ctx->id.execution != scope->expected_execution ||
            ctx->allocation.domain != scope->expected_actor_domain ||
            ctx->allocation.owner != scope->expected_actor_owner ||
            ctx->allocation.generation != scope->expected_actor_generation)
        return ORES_F32_HOST_WRONG_ACTOR;
    /* Rechecks physical OS thread, current generation, claim sequence and
     * RUNNING phase under the supervisor's native authority mutex. STOPPING
     * and a copied claim on another worker are both denied. */
    if (!ores_execution_owns_running(scope->physical_authority,
                                      scope->physical_claim))
        return ORES_F32_HOST_NOT_RUNNING;
    if (!lhs_rows || !lhs_cols || !rhs_rows || !rhs_cols ||
            lhs_rows > 32 || lhs_cols > 32 ||
            rhs_rows > 32 || rhs_cols > 32 ||
            lhs_cols != rhs_rows || max_output_bytes > 4096)
        return ORES_F32_HOST_BAD_SHAPE;
    const size_t output_bytes = lhs_rows * rhs_cols * 4u;
    if (output_bytes > max_output_bytes)
        return ORES_F32_HOST_QUOTA;
    return ORES_F32_HOST_PREFLIGHT_ONLY;
}
