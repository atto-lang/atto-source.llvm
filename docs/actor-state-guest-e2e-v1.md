# Actor-state E2E v1 — LLVM native AOT

**Status: AUTHORED_NOT_EXECUTED / NOT_CERTIFIED**

The source files `tests/fixtures/actor-state-e2e-v1/positive.atto` and
`sealed-direct-state-reject.atto` are identical Git blobs in the Java and
LLVM repositories (cross-repository identity verified when staged).

The positive guest exercises **two concurrently live actor instances**,
readiness, distinct mailbox inputs 3 and 39, mutation via critical-write,
distinct output observations, and awaited terminal completion. It must emit
one exact `ACTOR_E2E_OK|42` line. Any incorrect answer or extra output
is a failed oracle. The negative guest accesses sealed state directly and
must fail at compile time with a state-access-related diagnostic.

To run on a trusted source checkout with a pinned head:

```bash
EXPECTED_HEAD="$(git rev-parse HEAD)" bash scripts/test-actor-state-guest-e2e-v1.sh
```

The runner exercises O0 and O2 executable lanes (16 repetitions each), validates source blobs and tool prerequisites,
and prints SHA-256 identities for artifacts when successful.
Compiler support is **not assumed**: a rejected positive fixture is a
real parity capability gap, not a passing skip. An unclassified negative
diagnostic also fails. No synthetic evidence counts as a guest pass.

Do not merge based on an unstarted/zero-step GitHub Actions run. A separate
read-only evidence verifier must bind exact commit/tree/workflow/run attempt,
allocated runner, completed build/guest steps, compiler and guest hashes,
independent stdout oracle and the actual negative compiler result.

Private source may only be mirrored to an explicitly authorized private
carrier after full tree/blob equivalence checks; never use public mirrors
or expose secrets to untrusted workflow code.
