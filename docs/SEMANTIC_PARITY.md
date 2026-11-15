# Applicable backend parity

Parity means matching guest-visible language rules within supported features.
It does not require identical runtime machinery.

| Area | Shared contract | Permitted backend differences |
| --- | --- | --- |
| Borrowing | Exclusive writers; compatible shared reads; invalid loans cannot grant scheduling permission | Native mutable descriptors must be revalidated; Java records are immutable |
| Logical DOP access | Store, archetype, component, field, half-open rows and epoch determine overlap | AoS, SoA and AoSoA physical byte geometry |
| Actor ownership | Actor-local mutable state and checked transfer/isolation boundaries | Separate native arenas versus managed allocations with enforced isolation |
| Reclamation | No guest-visible use after lifetime ends | GraalVM GC versus native deterministic reclamation; host JVM allocations are allowed |
| Compilation | Same supported program behavior and diagnostics where specified | GraalVM/JVM execution and native LLVM AOT pipelines |
| Numeric domains | Nonnegative signed-64-bit rows/epochs form the portable policy domain | Native unsigned-64-bit policy APIs may support larger values |

These are policy targets, not a claim that all compiler/runtime integrations or
Forge guest actor lowering already implement them. Source-level negative tests,
actor lifecycle tests and compiled-program comparisons remain separate gates.

## Executable logical-borrow comparison

`verification/DopPolicyProbe.java` in the Java repository and
`verification/dop_policy_probe.cpp` in LLVM call the production policy directly.
The byte-identical script `scripts/test_dop_policy_parity.py` in both repositories
builds either probe, generates a deterministic shared corpus and checks every
result against a separate Python model. It requires no GraalVM or LLVM SDK.

```sh
python3 scripts/test_dop_policy_parity.py --java-root /path/to/oreslang-source.java --llvm-root /path/to/oreslang-source.llvm
```

For a single repository, supply only its root flag. Java needs Java 17+ with the
`jdk.compiler` module; C++ needs a C++17-capable `g++`.

The corpus has 27,140 ordered comparisons: hierarchy wildcards and siblings,
all layouts and access modes, distinct stores, half-open range boundaries,
stale epochs, signed-long limits, blank identities (including Java Unicode
whitespace), and malformed descriptors. Identity fields use hex UTF-8 on the
probe wire so tabs and newlines are tested without changing record boundaries.
Both directions are tested. Valid pair results must also agree with singleton
scheduling, internal-loan validation and epoch validation. Separate startup
regressions reject invalid entries hidden in empty/singleton access sets, and
native mutations to range, identity, hierarchy, layout and access mode.

Java snapshots input lists with `List.copyOf`, rejecting null elements before
vacuous loops or early conflict returns. Native public descriptors are checked
at decision boundaries. Native callers must still avoid data races while a
policy call reads their descriptors; validation is not synchronization.

Local validation used OpenJDK 17 and GCC 13. It does not exercise Truffle,
GraalVM native-image, LLVM code generation, or full compiler integration.
