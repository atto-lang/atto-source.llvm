# AttoLang LRU → LLVM native microtrace conformance (M0)

**Status: SOURCE-ONLY / NATIVE COMPILER GUEST NOT_EXECUTED.**

This source-local slice exercises four prerequisites of the fixed-capacity
AttoLang cache without copying private source from another GitHub organization
and without pretending that the full LRU class has been compiled.

| CTest | Input | Required native exit | Gate |
|---|---|---:|---|
| `atto_lru_hash_quotient` | `hash-quotient.atto` | 42 | Bounded hash quotient at signed 32-bit maximum; no removed remainder operator |
| `atto_lru_probe_wrap` | `probe-wrap.atto` | 17 | `Array<int>` probe collision, wraparound, independent slot values |
| `atto_lru_slot_recycle` | `slot-recycle.atto` | 29 | Two SoA columns; scrub and reuse a logical slot without changing neighbor |
| `atto_lru_recency_touch` | `recency-touch.atto` | 61 | Intrusive recency predecessor/successor consistency after tail promotion |

All four files live at `contracts/atto-lru-llvm-v1/`. Each native CTest
uses the preexisting `oreslang_contract_test` harness to invoke the **real**
`oreslang-llvmc`, inspect emitted LLVM IR, link via Clang and execute a
native process with an exact, nontrivial expected exit code. The three array
tests link the source repository's `NativeIntArrayRegion.c`. A CTest
registration is **an authored test only**, not a passing execution receipt.
The four exact fixture Git blobs are pinned by
`tests/atto_lru_llvm_source.py`, which deliberately rejects seven source/CI mutations in a scratch
directory: four modified fixture bodies, a symlinked source, an incorrect
native exit oracle, and a missing primitive-array runtime linker input.
It rejects commented-out or duplicate native CTest registrations.
This source integrity gate does not invoke the LLVM compiler.

This is a **narrow primitive/scalar prerequisite**, not full LRU parity:
there is no compiled native `IntLruCache` class, named constructor,
`Option<int>` API, critical section/actor ownership, AoSoA tile allocation,
memory-layout measurement or seven-fixture cache differential result.
Java/GraalVM and LLVM native execution remain independently unverified.
Do not label Python reference models or source-only inventories as backend
execution evidence.

## Promotion gate

For the exact LLVM PR head, record Git commit/tree, compiler executable
SHA-256, CMake configure/build stdout/stderr, a nonzero-step hosted runner,
all four CTest commands and guest exit values, Clang version and compiled
binary identity. Each failure must remain a failure; do not convert a skipped
or unlaunchable compiler into a pass. Repeat with sanitizer and optimization
configurations. Only after these pass should native class/Option integration
and the seven existing AttoLang LRU guest fixtures be attempted, with
independent ownership and cancellation validation.

## Cross-organization source boundary

The original LRU source is in `lru-cache-atto/lru-cache.atto`; these LLVM-local
microtraces are newly authored and contain **no mirrored private repository
tree**. Source-owned LLVM CI currently does not provide a stepful guest
receipt. A source-free private carrier can test runner availability but
cannot validate LLVM guest compilation without separately approved source
transfer. The private publisher governance issue remains
https://github.com/ores-truffle-oreslang/oreslang-source.java/issues/962.

## Source-organization exact-head execution workflow

`.github/workflows/atto-lru-llvm-m0.yml` adds a focused **source-local**
LLVM CI job that checks out the immutable PR head (not an automatically
generated merge tree), checks the source/tree identity before build, installs
LLVM and Clang, verifies all four pinned fixture Git blobs plus seven
negative source/CTest mutations, builds the **actual** `oreslang-llvmc` binary,
and verifies exactly five registered CTests. It runs the four real compiler
→ LLVM IR → Clang → native guest programs, plus the source integrity check,
and requires a CTest JUnit report with exactly five distinct, successful,
non-skipped cases. A zero-test or unlaunched compiler cannot produce the
`ATTO_LLVM_LRU_GUEST_EXECUTION=PASS` marker. The workflow rechecks the
source tree after tests and captures the CTest inventory/JUnit/logs in a
private source-owned artifact.

`tests/test_atto_lru_workflow_contract.py` checks that these controls are
present and tests fail-closed behavior for a no-op compiler build, missing
native CTest, removed skip detection, unpinned checkout and an injected
`continue-on-error` bypass. These are **host/static workflow policy tests**,
not compiler-backed guest results.

The current source organization has private Actions jobs ending with
`steps=null`. Workflow YAML and test definitions alone do not change
the evidence classification; the new job must actually execute nonempty
steps on its exact head and pass before declaring even this *narrow*
four-microtrace LLVM slice. The seven complete LRU cache fixtures and
Java/LLVM cross-backend parity remain separate unverified milestones.


## Additional native AoS / AoSoA microtraces

Four independently authored `.atto` compiler-to-LLVM-IR-to-Clang CTest candidates:
`aos-triple-reuse.atto` (expected exit 43), `aos-pair-occupancy.atto` (44),
`aos-padded-zero.atto` (45), and `aosoa-tail-reset.atto` (46).
The last poisons a non-logical tail lane, resets all physical cells, and reuses a slot.
All link the existing native integer-array runtime.

These are scalar/array source microtraces, **not** execution of the complete
`IntLruCache` class or proof of a compiler-inferred AoS, SIMD, ABI, zero-GC,
or native memory-lifetime guarantee. They require stepful exact-head compiler,
IR verifier, native link, and guest execution before becoming parity receipts.
