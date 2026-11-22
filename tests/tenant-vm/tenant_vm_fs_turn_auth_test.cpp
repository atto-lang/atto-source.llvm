#include "oreslang/runtime/TenantVmFsTurnAuth.hpp"
#include <cassert>
#include <cstdint>
#include <cstdio>

using oreslang::runtime::vm::internal::FsTurnAuth;
using oreslang::runtime::vm::internal::actor_fs_span;
using oreslang::runtime::vm::internal::next_fs_incarnation;
using oreslang::runtime::vm::internal::same_fs_turn;
using oreslang::runtime::vm::internal::scratch_fs_span;
using oreslang::runtime::vm::internal::valid_fs_turn;

int main() {
    // Actor slot zero with generation zero is valid; VM generation and the
    // distinct actor incarnation must be nonzero and host-controlled.
    const FsTurnAuth granted{51u, 0u, 1u, 17u,
                              4096u, 65536u, 49152u, 16384u};
    assert(valid_fs_turn(granted));
    assert(same_fs_turn(granted, granted));

    auto sibling = granted;
    sibling.actor_ref = 1u;
    assert(!same_fs_turn(granted, sibling));
    auto other_vm = granted;
    other_vm.tenant_generation = 52u;
    assert(!same_fs_turn(granted, other_vm));
    auto restarted = granted;
    restarted.actor_incarnation = next_fs_incarnation(granted.actor_incarnation);
    // Actor ref intentionally remains the same across this restart.
    assert(restarted.actor_ref == granted.actor_ref);
    assert(!same_fs_turn(granted, restarted));
    auto later_turn = granted;
    later_turn.turn_sequence = 19u;
    assert(!same_fs_turn(granted, later_turn));
    auto even_turn = granted;
    even_turn.turn_sequence = 18u;
    assert(!valid_fs_turn(even_turn));
    auto no_vm = granted;
    no_vm.tenant_generation = 0;
    assert(!valid_fs_turn(no_vm));
    auto denied_incarnation = granted;
    denied_incarnation.actor_incarnation = 0;
    assert(!valid_fs_turn(denied_incarnation));

    // Actor-private span validation is separate from scratch-private result
    // publication. Never allow another actor's region or 32-bit wraparound.
    assert(actor_fs_span(granted, 4096u, 1u));
    assert(actor_fs_span(granted, 49152u, 16u));
    assert(!actor_fs_span(granted, 4095u, 1u));
    assert(!actor_fs_span(granted, 69631u, 2u));
    assert(!actor_fs_span(granted, UINT32_MAX, 1024u));
    assert(scratch_fs_span(granted, 49152u, 128u));
    assert(scratch_fs_span(granted, 65536u, 0u));
    assert(!scratch_fs_span(granted, 49151u, 1u));
    assert(!scratch_fs_span(granted, 65536u, 1u));
    assert(!scratch_fs_span(granted, 69632u, 1u));

    auto corrupt_region = granted;
    corrupt_region.actor_offset = UINT32_MAX;
    corrupt_region.actor_bytes = 64u;
    assert(!valid_fs_turn(corrupt_region));
    auto escaped_scratch = granted;
    escaped_scratch.scratch_bytes = 65536u;
    assert(!valid_fs_turn(escaped_scratch));

    // No wraparound resurrection after the final incarnation.
    assert(next_fs_incarnation(1u) == 2u);
    assert(next_fs_incarnation(UINT64_MAX) == 0u);
    assert(next_fs_incarnation(0u) == 0u);
    std::puts("ATTO_VM_FS_SCHEDULER_TURN_AUTH_HOST_ONLY_PASS");
    return 0;
}
