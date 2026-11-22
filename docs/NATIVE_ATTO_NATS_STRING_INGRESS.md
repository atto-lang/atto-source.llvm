# Native AttoNATS ingress String literal predicate — LLVM host AOT

This compiler tranche adds the **real native** `String.contains_literal(String)`
method for the LLVM-host backend. It is a general String primitive used
by `src/core/subjects.atto` in AttoNATS to reject whitespace/control
tokens during publish/subscribe validation. It does **not** implement a
NATS router, network server, authentication policy, or a complete
`String.split_literal` backend.

## Source semantics

```atto
use namespace guest;

pub routine main(): int {
    val String subject = "orders.created.eu";
    return subject.contains_literal("created") &&
      subject.contains_literal(".") &&
      !subject.contains_literal(" ") ? 0 : 1;
}
```

- The receiver and needle must both statically be `String`.
- Exactly one positional needle argument is admitted; extra, missing or
  named arguments fail before LLVM IR is emitted.
- The implementation accepts arbitrary bounded byte spans (including
  embedded NUL and UTF-8) and searches *bytes*, not Unicode
  code points/graphemes. Empty needle returns true.
- String handles remain internal to the current lexical invocation
  region. They cannot be integer-forged, persisted between invocations,
  or handed to other actors through this API.
- A no-match result is false; a stale/invalid handle poisons the String
  region, so finalization fails. No wrong-identity result authorizes
  NATS delivery.
- No alloc/free/GC, mutable borrow, or external I/O occurs in the native
  substring search itself. Whole guest execution still has region
  bookkeeping and String-literal allocation. Unbounded string search
  is NOT certified hard real-time; the RT analyzer conservatively flags
  native ArrayCall as allocation/unproven.

The checker allows this specific read-only operation in a pure
conditional guard and rejects other impure native array/member calls.
The compiler lowers to `ores_string_contains_literal(i64, i64) -> i64`
with a boolean 0/1 result, preserving input evaluation order.

## Tests and evidence scope

- `native_string_contains_literal_binary_unit` links the actual
  `NativeStringRegion.c` into a C executable and tests empty/nonempty
  patterns, embedded NUL, search boundaries, repetitive prefixes,
  and poisoned invalid handles.
- `tests/native_class_string_parity.rs` adds an executable Atto guest
  using `String.contains_literal` inside NATS-style validation,
  plus four **no-IR** compiler-negative cases (receiver, needle,
  arity, named argument).
- [LLVM #1714](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/1714)
  separately pins exact native AttoNATS pretokenized matching source
  and requires LLVM IR / Clang O0/O2 guest execution. This PR is
  stacked on that draft and extends backend capability rather than
  claiming the entire application works.

**Execution gate:** these new exact-head C/C++ compiler and native
runtime tests still require GitHub Actions jobs with nonzero steps.
The source organization's recent Actions failures happen before
runner admission. No private LLVM compiler source should be newly
exported across organizations without the protected-source approved
publisher and security-review process.

## Next ingress blocker

Full `src/core/subjects.atto` validation also uses
`String.split_literal(".")`. A correct native `split_literal`
must construct a uniquely owned `Array<String>` with bounded bytes
and tokens and transfer region/drop authority through parser, type
checker, ownership checker, LLVM IR and runtime. This method has NOT
been added by the contains-literal tranche. It must not be emulated
with a host-side NATS parser, unowned pointers or a hidden global
cache. It precedes executable authenticated actor-owned subscription
state and native async NATS PUB/SUB/MSG acceptance.
