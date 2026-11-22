# Native / Graal parity: implicit ownership and actor heaps

The canonical language contract treats ownership as an inferred property, **not**
a public `owned` type modifier. The LLVM backend's experimental
`const x: owned int` spelling is an internal compiler test fixture only. It
must not be incorporated into public examples or used as an ABI requirement.

## Parity gates before production

| Capability | Java/Graal | LLVM | Gate |
|---|---|---|---|
| Typed immutable locals | Supported | LLVM lexical stack slots | Test same source text and return values |
| Ownership annotations | Ownership/borrow checker | Internal `owned int` fixture | Remove public syntax dependency; select stack versus region from type/liveness analysis |
| Native actor data | Java 25 FFM `Arena.ofShared()` behind actor ownership | Actor-owned i64 heap | Compile ordinary actor fields/arrays to these regions; deny foreign handles |
| Suspended native loans | Quiescent FFM close | Loan-gated actor heap | Release only after terminal actor state AND final outstanding loan |
| Actor single-flight | Logical actor admission independent of carriers | Native critical tickets and actor-memory turn admission | No parent/supervisor or child user code concurrently in one lane |
| Real HTTP / async channels | Java runtime support under integration | Incomplete native frontend/runtime | Same source HTTP server, cancellation/select, p50/p99 and GC/native-heap metrics |
| Accelerator actors | DOP layout/kernels emerging | CPU/OpenCL geometry and fences emerging | Same typed IR, async buffer lifetime proof; no simulated hardware claims |

## Explicit known incompatibility

Current LLVM `Parser.cpp` reserves `owned`, accepts it in local declarations,
and `docs/NATIVE_OWNED_SCALAR_HEAP.md` shows it as source syntax. This conflicts
with the canonical shared language contract. Preserve the experimental native
test coverage while replacing the source-facing modifier with compiler-inferred
storage class. Do not silently introduce this keyword into the Java lexer.

## Required tests before merging a lowering rewrite

1. Parse the **same ordinary Oreslang source** in Java and LLVM, including
   `const value: int = 6`, nested scopes and calls.
2. Verify LLVM emits no tracing-GC statepoints and deterministic region
   retirement; do not infer this merely from a `-O2` allocation benchmark.
3. Check double drop, use-after-move, forged handles, stale generations,
   overflow, suspension with outstanding loans and cross-actor access.
4. Run native CTest under ASan/UBSan plus Release and Java FFM loans on GraalVM25.
5. Compare sustained native and JVM actor HTTP workloads, including
   GC pause counts, native memory retention and actor shutdown latency.
6. Verify any funded-org CI mirror against **exact current source blobs** before
   claiming evidence; never use an unexecuted zero-step Actions result.

This document reports a verified integration gap; it does not claim
source-level actor-field lowering or production parity.
