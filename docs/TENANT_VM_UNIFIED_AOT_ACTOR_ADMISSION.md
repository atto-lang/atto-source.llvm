# TenantVM native source-AOT + actor-turn admission: unified review gate

**Status:** DRAFT integration; actual source/native execution is **NOT_EXECUTED**
until the current SHA runs have real checkout, compiler and executable steps.
This is **NOT** a native sandbox for untrusted actors or evidence of
GraalVM/V8 equivalent memory isolation.

## Exact code composition (inside original private repository)

| Component | Required provenance |
|---|---|
| Original actor host foundation | #1890 → #1892 → #1897, exact #1897 head `03c6c9210c1650410db845121f3dd2836d2ba2ca` |
| Compiler/AOT guest foundation | #1896, exact head `c9932d30c94f8bc1dab5a41b501468d34ed99599` |
| Base actor Git tree | `693a7ab418dee84939578f72306a1d3930e58b95` |
| AOT source blobs applied to that tree | `.github/workflows/tenantvm-atto-source-aot-admission.yml`, `docs/TENANT_VM_SOURCE_AOT_SECURITY_ADMISSION_V1.md`, `src/main/cpp/oreslang/backend/LlvmCodegen.cpp`, `src/main/cpp/oreslang/main.cpp`, `tests/fixtures/tenantvm-trusted-scalar.atto`, `tests/native_tenantvm_atto_guest_test.cpp`, `tests/tenantvm_atto_source_aot_gate.sh` |
| Initial integrated tree | `fa0d7bbae5b989bd2e417fed02b9dc79c7ac5d64` |

All copies used Git *blob IDs within* `ores-truffle-oreslang/oreslang-source.llvm`.
No Git objects were mirrored to a different organization. The integration
test deliberately modifies only the integration PR's own source fixture and
workflow, leaving the underlying feature PRs independently reviewable.
The source identity test locks the exact compiler and runtime blob IDs until
a new explicitly reviewed head replaces them.

## What this combined host+guest test proves when executed

`tests/tenantvm_atto_source_aot_gate.sh` builds the actual `oreslang-llvmc`
source fixture `tenantvm-trusted-scalar.atto` into checked LLVM IR, uses
Clang at O0 **and** O2 and links both C++ host harnesses against the resulting
native guest object.

The original source guest harness checks checked overflow, ABI rejection,
independent tenant globals, concurrency and shutdown. The new
`tests/native_tenantvm_integrated_actor_source_test.cpp` checks the
physical-turn/object boundary and **real** `ores_guest_entry` in one
executable: a carrier with an active actor turn must not use the generic
unscoped native guest entry, including into another tenant. A released
turn permits ordinary trusted scalar entry again without changing any
unsuccessful nested call's state. A callback inside the generic guest entry
cannot acquire an actor turn. Neither test grants guest code an actor ticket
or raw object address.

The companion `tests/tenantvm_actor_turn_exact_checkout.sh` builds
actual OresVM host tests under GCC and Clang, optimized/NDEBUG and
ASan/UBSan. The synthetic `tests/tenantvm_actor_turn_gate_selftest.sh`
tests only the **test harness**, not real OresVM.
The workflow invokes `tests/tenantvm_unified_source_identity.sh` before
compilation, requiring a clean checkout, exact head, regular Git blob modes
and matching immutable content identities.

## Explicit remaining risks

1. This compiler's source `--isolate-entry` is still a *trusted-only*
   integer state transition; there is **no compiler-generated actor object
   handle lowering, actor-turn-aware guest ABI or fully sealed actor heap**.
2. Host-issued object tickets and thread-local turn admission prevent
   accidental host API misuse, **not arbitrary native pointer dereferences**.
3. Untrusted tenant entry must remain rejected before source reads/IR until a
   reviewed OS-process / memory-safe execution boundary exists. Disabling
   FFI inside the source language alone is not a containment proof.
4. A host caller abandoning a physical turn after an exception must not
   blindly pass the ticket to another carrier. A reviewed scoped-turn
   release / supervisor recovery contract remains follow-up work.
5. Actor scheduler integration, checked reference lowering and executable
   multi-actor guest fixtures are future gates, not demonstrated by this PR.
6. Source GitHub Actions and the most recent source-free carrier probes
   failed before any job steps. **Zero-step failures mean NOT_EXECUTED**
   rather than failing compiler tests or proof of exhausted paid minutes.
7. Cross-org source export remains denied by source governance while
   `main` is unprotected, carrier contracts do not authorize this PR, and
   the unresolved private-source retention incident #1150 remains open.
   A token-free Git API mirror is still a private-source transfer.

## Required next sign-off

Record, for the current integration head, nonempty exact Git checkout
and actual GCC/Clang sanitizer logs for the actor-turn test, native
`oreslang-llvmc` build and executed O0/O2 generated guest harness
results. Obtain independent security review, resolve private carrier
governance, and keep all PRs draft until successful evidence exists.
