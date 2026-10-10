# Native owned scalar region: strict no-tracing-GC heap lowering

Oreslang's experimental LLVM backend now supports:
```oreslang
pub fnc main(): int {
  const value: owned int = 6;
  val result: owned int = value * 7;
  return result;
}
```

`owned int` is an explicit heap-backed scalar owner. Unlike `int`, which
lives in a stack `alloca` with LLVM lifetime hints, every `owned int`
is allocated in a bounded native region created for the function call.
The compiler copies scalar values to return/call arguments, so no native
heap pointer escapes this restricted source subset.

The verified LLVM IR calls `oreslang_owned_i64_region_open`,
`oreslang_owned_i64_region_new`, emits exactly one
`oreslang_owned_i64_region_drop` per successfully created owned binding in
reverse declaration order, and finally `oreslang_owned_i64_region_close`.
The native runtime validates pointer-to-region provenance and live counts;
double release, wrong-domain release, region-close-with-live-slots, invalid
size and allocation failure abort rather than freeing memory unsafely.

The region uses one native `calloc` and `free` per activation, not a tracing
collector. There are no GC statepoints, GC annotations or collector hooks in
generated guest IR. A compiler-side verifier rejects unknown external and
indirect calls. The permitted native ABI names are exact and compiler emitted.
A validated native executable must complete normally: a missing drop causes
the region-close guard to abort instead.

## Restrictions

- The region currently belongs to a **function activation**, not an actor.
  It cannot survive the return or be shared between actors. The OresVM actor
  scheduler/native arena bridge and borrow-to-actor-lifetime lowering are next.
- Only `owned int` works. No user class, struct, destructor body, field,
  array, string, aliasing native pointer, borrow across await, exception or
  cancellation is accepted by this compiler subset.
- The compiler now also admits a **restricted early-return CFG** after all
  scalar local bindings are initialized: `if (int_expr eq int_expr) {
  return int_expr; } fi`, repeated zero or more times before the mandatory
  final `return`. Every admitted exit computes its result then drops ALL
  initialized owned scalar bindings in reverse order and closes the lexical
  native region. The current branch has no conditional locals, nested blocks,
  `else`, mutation, exceptional unwind, `throw`, `panic`, `defer`,
  `trap`, `await` or cancellation lowering. Unsupported source must reject
  without emitting LLVM IR. Partial initialization/drop flags and general
  CFG cleanup remain future work; this is not general source-level RAII.
- Native `calloc/free` may contend or page fault; *zero tracing-GC pauses*
  is not a claim of constant-time allocation or general real-time execution.
- Native code outside this compiler-generated guest ABI must be independently
  audited before end-to-end OresVM strict no-GC certification.

## CI

The funded exact-source test mirror builds Release LLVM CMake, runs all 43
CTest contracts, including native execution of nested owned regions, validates
compiler rejection of unsupported types, and inspects IR for reverse drops,
closure and absence of tracing-GC statepoints.

## Guarded early-return example (experimental)

```oreslang
pub fnc main(): int {
  const a: owned int = 6;
  val b: owned int = 7;
  if (a eq 6) {
    return a * b; // native drop(b), drop(a), close(region), return 42
  }
  fi
  return 0;       // also emits drop(b), drop(a), close(region)
}
```

The guard compares two `int` expressions using canonical `eq` or the
compatible `==` alias, and requires the Java-compatible `fi` terminator.
It does not
permit integer truthiness or arbitrary boolean constructs. Runtime tests must
exercise the taken, not-taken and chained paths, verify all per-path drop/close
calls, and reject branch-local ownership or missing final returns before LLVM IR
emission. Native ABI `close` refuses live objects, providing an executable
check against omitted drop calls in the accepted slice.

These fixtures are designed as a bounded stepping stone to an explicit
control-flow ownership/dataflow pass, not a replacement for one.
