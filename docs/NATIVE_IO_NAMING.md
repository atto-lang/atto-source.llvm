# Native Oreslang I/O naming and execution contract

**Status:** normative language/API and scheduling decision; the Java/Graal feature branch implements canonical native `_blocking` entry points with an opt-in strict compiler naming gate, but default enforcement and versioned release are **pending**. LLVM does not yet implement native I/O or continuation lowering: its compiler must fail closed. This document does not claim full cross-backend conformance or production readiness.

## Rule

Every **source-visible, logically synchronous I/O operation** must have an identifier ending in `_blocking`. This name means the *actor* waits for the result, **not** that the actor carrier may block. Async I/O operations **may** end in `_async`, but that suffix is **not required**. This is an API and scheduler contract, not a new operator, parser keyword, or declaration syntax.

| Kind | Rule | Examples |
| --- | --- | --- |
| Synchronous-looking, continuation-suspending I/O | `_blocking` suffix required | `fs.read_text_blocking(path)`, `fs.list_dir_blocking(path)`, `http.get_text_blocking(url)`, `stdio.println_blocking(value)` |
| Async I/O | `_async` suffix optional | `await fs.read_text_async(path)`; alternatively an explicitly async API without that suffix |
| Pure/local computation | No I/O suffix required | `calculate_hash(data)`, `parse_header(data)` |

A `_blocking` I/O operation is sequential from the source actor's point of view. At runtime it **must submit I/O to a non-carrier execution mechanism, suspend the actor continuation without parking an OS scheduler carrier, and resume the continuation on its owning scheduler**. This applies to filesystem metadata, terminal/stdin/stdout, sockets, DNS, networking and I/O usually expected to complete quickly. An implementation may use an OS asynchronous API, completion queue, bounded virtual-thread I/O worker, or another independently scheduled I/O engine. A blocking host syscall on a separate bounded/virtual I/O worker is a portability fallback; it may pin an I/O worker but never the actor carrier.

The name alone is not proof of a safe implementation. A function named `*_async` is not asynchronous merely because of the suffix; it must expose an async/future execution contract and must not block the actor carrier while performing I/O. Conversely, `*_blocking` is an API requirement for a synchronous-looking result, **not permission to call blocking host I/O on an actor carrier**, nor permission to bypass capability checks. It must not call `Future.join()`, `Future.get()` or a spin loop on the actor carrier.

The rule applies to exported Oreslang standard-library/native I/O functions, module functions, object methods and user-defined I/O wrappers that directly expose a blocking I/O operation. It does **not** rename host C/C++/Rust functions, ordinary compute functions, or every higher-level workflow routine that happens to *call* an I/O function; language entrypoints such as `main` keep their specified names.

## Examples

```ores
// Synchronous to this actor: continuation yields; its OS carrier stays available.
val String text = fs.read_text_blocking(path);

// Async call: the actor can suspend and let its carrier do other work.
val String text = await fs.read_text_async(path);

// Async names may omit the suffix only when the async signature and behavior
// are explicit and unambiguous; e.g. a future stdlib contract.
val String text = await fs.read_text(path); // only if declared async

val int sum = add(1, 2); // ordinary computation, no suffix
```

The examples describe the **target** API, not a claim that all names above are accepted by today's compiler/runtime.

## Semantic requirements

1. **Classify by semantic effect and execution behavior.** The native built-in/FFI registry must record I/O kind (`io` versus non-I/O), result shape (`T` for `_blocking`; `Future<T>` for async) and suspension requirements. A name-only regex over arbitrary functions is not sufficient. Calls to `_blocking` must be lowered as suspension points even inside ordinary (non-async) source routines.
2. **Reject missing suffixes.** Whenever a native I/O import, builtin or generated binding is resolved, an entry marked synchronous-looking I/O without `_blocking` must produce a stable compiler/binding diagnostic. The check must also cover methods and aliases. Do not infer the effect solely from the suffix.
3. **Both forms release the carrier.** An async I/O entry must have the correct future/awaitable contract and submit without blocking the actor carrier; `_async` is merely conventional. A `_blocking` entry must return `T` in source but be lowered into the same continuation-suspending completion path, without requiring an explicit `await`. Reject/flag a falsely annotated binding.
4. **Isolation and permissions remain in force.** Both variants require the same relevant capability authorization. Async cancellation/completion and memory ownership need separate validation. Neither spelling grants privileges.
5. **Uniform source contract across backends.** LLVM AOT, Java/Graal JIT, and hybrid execution must expose consistent compliant source names when the relevant capability is implemented. An unsupported capability still fails closed.

## Migration and compatibility

The Java/Graal runtime currently documents bare synchronous names (including `fs.list_dir`, `fs.is_dir`, `fs.is_symlink`, `fs.write_text_atomic`, and `stdio.println`). They are **existing implementation behavior, not compliant canonical names**. The LLVM compiler currently lacks general filesystem/network stdlib I/O.

- Add canonical `*_blocking` declarations to builtins and generated bindings, and update documentation, compiler fixtures and downstream code.
- Migrate uses in CLI/code generators and validate each affected code path with real execution tests before removing bare aliases.
- Do not silently reclassify the old blocking calls as async. If a temporary legacy mode is necessary for compatibility, gate it explicitly and emit a migration diagnostic; do not advertise legacy names as compliant native APIs.
- Version/pin API contracts so a released toolchain and its generated sources agree on names. Roll back the canonical-name switch via a versioned compatibility gate rather than invisible aliasing.
- Before production enforcement, add positive and negative regression cases (below) to each backend supporting I/O. Reconcile actual Java/Graal and LLVM compiler support separately.

## Acceptance tests

| Scenario | Expected |
| --- | --- |
| Registered synchronous-looking FS/HTTP/TCP/stdio I/O API ending in `_blocking` | Admitted only when compiled to async-backed continuation suspension, subject to capability grants |
| Registered blocking I/O API called or exported without `_blocking` | Rejected with an explicit blocking-I/O naming diagnostic |
| Blocking method or alias hides a bare synchronous I/O entry | Rejected; aliases cannot bypass the check |
| Registered async I/O API named `*_async` | Admitted when its async execution and future contract are valid |
| Registered async I/O API without `_async` | Admitted when declared and implemented as async |
| `_blocking` or fake `*_async` API that blocks the carrier | Rejected at binding validation or caught by single-carrier concurrency integration test |
| Pure function without either suffix | Admitted |
| Unknown/unsupported I/O builtin or capability | Fails closed; no silent host fallback |

Testing the naming validator alone is insufficient to establish non-blocking scheduling, fault isolation, secure path resolution or cancellation safety. The mandatory test launches two actors on **one** scheduler carrier; actor A performs slow `_blocking` I/O while actor B must make progress before I/O completion. Verify error propagation, timeouts, backpressure, actor cancellation, shutdown, and that no guest callback runs on an I/O worker. Each backend must additionally execute blocking and async operations under concurrency and denied-capability cases.

## Evidence and responsibility

The common source repository owns this contract. The separate LLVM and Java/Graal repositories own compiler/builtin/runtime implementation and executable tests for now; do not merge their sources just to adopt this rule. Record versioned conformance evidence and a migration/rollback status for each backend before declaring completion.

## Implementation and verification receipts (2026-10-09)

| Component | Executable evidence | Release limitation |
| --- | --- | --- |
| Java/Graal [PR #522](https://github.com/ores-truffle-oreslang/oreslang-source.java/pull/522) | [GraalVM/JDK 25 run #38005034460](https://github.com/athlet-o/athleto-e2e/actions/runs/38005034460): 20 JUnit cases plus 4 real CLI command tests, all passed; single-carrier actor progress, deny-all authority, strict naming, first-class `_blocking` alias refusal and nested tail-call continuations covered | Strict naming is opt-in; legacy bare entry points remain available pending versioned client migration; broader compiler matrix and distribution tests remain |
| Native Oreslang Stack CLI [PR #12](https://github.com/ores-truffle-oreslang/oreslang-stack-cli/pull/12) | 69 source-level native I/O sites migrated; the exact CLI source passed strict typechecking and actual init/check/routes/deploy-denial runtime checks in the same GraalVM run | Cannot merge until the compiler dependency is released and pinned; deployment/hot reload and self-contained build artifacts are separate features |
| LLVM [PR #29](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/29) | [LLVM 18 CTest run #38005361640](https://github.com/athlet-o/athleto-e2e/actions/runs/38005361640): both bare and suffixed unimplemented I/O calls rejected with no IR emission | No native I/O registry, async ABI, typed continuation lowering or runtime I/O semantics exist; negative tests are not implementation parity |

**Mandatory production cutover:** pin a shared contract version in generators and compilers; migrate remaining bare stdlib users; enable strict naming by default for that version with a separately versioned legacy compatibility mode; run actor isolation, cancellation, backpressure, timeouts, context teardown and full backend conformance. Do not enable LLVM native I/O by spelling alone or silently park an actor carrier.
