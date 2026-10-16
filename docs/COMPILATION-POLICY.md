# Compilation, trust and backend policy

Status: agreed future requirements and proposed implementation paths. This document does not certify current backend support.

## Three independent dimensions

1. **Trust:** trusted application code versus untrusted/dynamically supplied guest code.
2. **Actor isolation:** shared versus isolated actor execution.
3. **Compilation:** AOT, JIT, or hybrid execution.

An isolated actor is not automatically untrusted. JIT compilation does not grant trust. AOT compilation does not establish sandbox safety. The word “guest” in Truffle terminology also does not by itself mean security-untrusted code.

## Required direction

| Execution category | LLVM goal | Java/Graal goal | Policy |
| --- | --- | --- | --- |
| Trusted non-actor code | Complete program-code AOT required | Equivalent guest-code AOT target | Must eventually run without guest JIT or interpreter fallback on LLVM |
| Trusted shared actors | Complete AOT required | Complete AOT target | Actor sharing does not require JIT |
| Trusted isolated actors | Complete AOT required | Complete AOT target | Isolation is independent of compilation mode |
| Untrusted guest actors/functions | JIT, interpretation under policy, or optional AOT | Same permitted strategies | AOT is not required; sandbox and metering remain mandatory |
| Trusted strict realtime profile | Precompiled native execution; independently verified bounds | No equivalent hard guarantee presumed for managed execution | No runtime JIT in the strict region |
| Hot-loaded trusted code | Validated precompiled generation or explicitly authorized JIT | Same policy goal where supported | Strict AOT accepts compatible precompiled generations only |
| Trusted hybrid execution | AOT baseline plus optional optimization JIT | Equivalent target where integration supports it | Enabling JIT cannot change language semantics |

## Meaning of complete trusted AOT

For an AOT-only trusted program and its declared dependencies:

- Every reachable Oreslang function/routine implementation is compiled before execution.
- Actor run/receive/lifecycle methods and supervisors execute compiled Oreslang code.
- Async continuations, callbacks, loops, collection operations and scheduling checkpoints have compiled implementations.
- Closures, generics, dispatch tables and required metadata have an AOT-compatible representation.
- Execution requires no guest JIT, dynamic guest code generation or hidden interpreter fallback.
- Unsupported dynamic requirements are rejected at build/admission time, rather than silently interpreted.
- Hot loading, when enabled, validates separately precompiled generations and their compatibility.

Compiling a Java/Truffle interpreter with Native Image does not, by itself, satisfy this requirement. The Java/Graal experiment should make program-specific Oreslang operations visible to AOT compilation, for example through generated JVM classfiles.

Acceptance must exercise trusted shared and isolated actor programs with runtime compilation unavailable, verify that no guest interpreter fallback is present, and include async, dispatch, cleanup and cancellation cases. A startup smoke test alone is insufficient.

## Optional compilation policy sketch

The following TOML is illustrative future design, not supported configuration syntax:

```toml
[compiler]
backend = "llvm"              # llvm | graal
mode = "aot"                  # aot | hybrid | jit

[trusted]
aot = true
jit = "off"                   # off | auto | on
interpreter_fallback = false

[untrusted]
aot_required = false
jit = "auto"
sandbox = "strict"
```

Build-time contents and runtime policy must agree. A binary that excludes a JIT cannot enable one through a runtime flag. Strict-AOT deployment may reject dynamic uploads it cannot execute; supporting untrusted JIT must not force trusted code to use JIT.

JIT compilation itself needs bounded resources, cancellation and authorization for untrusted inputs. Executed code must obey capability, memory, CPU and isolation limits independently of compilation mode.

## Backend-specific strengths

| Capability | LLVM direction | Java/Graal direction |
| --- | --- | --- |
| Native arenas, explicit layout/alignment, stable native frames | Direct native implementation with checked lifetime contracts | Managed representations or explicit native adapters; no identical physical-layout promise |
| CPU affinity, NUMA, locked memory, DMA/RDMA and NIC bypass | Trusted supervisor-authorized resource services | Optional trusted bridge/broker; no ambient guest authority |
| Strict bounded allocation/no-GC regions | Compiler, runtime and OS admission must prove the profile | Managed execution does not inherit an equivalent guarantee |
| Java/Kotlin libraries and JVM tooling | Optional external integration | Natural host-integration path, with Native Image restrictions evaluated separately |
| Truffle polyglot integration | Optional bridge | Backend-specific host capability |
| Optional JIT | ORC-based path to investigate | Truffle/Graal path to investigate |
| Native diagnostics | Sanitizers and native profiling | JVM/Graal tooling and applicable native diagnostics |

Neither LLVM nor an annotation automatically guarantees realtime behavior, isolation, zero allocation or memory safety. These are independently testable contracts.

Portable types such as Future, Actor, List, Stream and Ref retain Oreslang-defined semantics. Host types such as CompletableFuture or ArrayList remain explicit interop values rather than the common language representation.
