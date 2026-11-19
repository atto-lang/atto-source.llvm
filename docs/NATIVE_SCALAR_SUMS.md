# Native Option and Result values

The LLVM backend admits `Option<T>` and `Result<T,E>` with `int`, `bool`,
or `String` payloads. `string` and `String` name the same payload type across
imports. These remain distinct checked types: neither their descriptors nor
their tags can be supplied as guest integers, booleans, class values, or sums
with different payload types.

```ores
fnc find(bool present): Option<String> {
  return present ? Some("id") : None;
}
pub routine main(): int {
  val Option<String> value = find(true);
  return value.is_some() && value.unwrap() == "id" ? 0 : 1;
}
```

Constructors use positional payloads and a declared contextual type. For a
conditional expression whose branches carry an Option or Result, both arms
must resolve to the same declared sum type; a raw scalar cannot stand in
for the other arm's checked descriptor. This is enforced in the source sum
resolver before LLVM IR generation, not left to native runtime tag checks.
Option
provides `is_some`, `is_none`, `unwrap`, and `unwrap_or`; Result provides
`is_ok`, `is_err`, `unwrap`, and `unwrap_or`. Fallback arguments evaluate
eagerly, matching Java. `Some(0)`, `Some(false)`, `Some("")`, and `Ok(false)`
are present values, not absence or errors. The current native subset does not
implement generic class payloads, nested sums, sum arrays, `trap`, `?`, or
panic recovery. Wrong-variant unwrap terminates with an explicit abort; it
does not yet provide Java's panic frames or lexical unwind behavior.

The compiler supports typed locals, value/borrowed parameters, results, and
class fields. Then-form early returns lower at their source position so a
following loop or effect cannot run before the guard. [Ordered scalar/sum
locals](NATIVE_ORDERED_LOCALS.md) can follow root statements; late owning
collections still require scoped drop lowering.

## Storage and checks

Each immutable sum uses a generation-checked descriptor in the bounded root
record region. A schema identifies its complete nominal payload signature.
Option stores a tag plus one payload slot; Result stores a tag and separate
success/error slots. Inactive slots are zero-initialized and are never read
as payloads. Construction scratch slots allocate once in the function entry
block, including constructors reached from loops.

Generated function boundaries validate descriptor generation, schema, exact
field count, and tag range before user code. Member operations repeat these
checks before inspecting an active payload. Allocation failure latches the
root region's failure state; zero descriptors cannot become successful sum
values. `ores_record_check_shape` also rejects stale, foreign-thread,
wrong-schema and wrong-arity descriptors. This is an internal typed ABI,
not an untrusted foreign-function interface accepting arbitrary payload bits.

Descriptors and dynamic strings are released deterministically at root exit;
there is no tracing GC, JIT, or source evaluation. This bounded synchronous
region is **not** a request-scoped allocator for a long-running server.
Actors, suspension and exception-frame integration remain rejected.
Sum descriptor checks are not certified allocation-free hard realtime work,
including in passthrough functions that never inspect the parameter.

## Verification and parity limits

`native_class_string_parity` runs the real `RequestPath` and `PathCapture`
definitions from http-routing, including capture bounds, `at`, and `one`.
It compares successful source programs and invalid unwrap behavior with a
pinned Java reference. It separately checks native-only syntax; literal
variant field defaults are currently native-only because the pinned Java
constructor effect checker rejects variant calls in defaults.

Release and sanitizer runs cover tags independently of zero/false/empty
payloads, imported signatures, eager fallbacks, fields, source-order guards,
and results surviving callee destruction. A C harness invokes generated
functions with invalid generations, schemas, field counts and tags, including
a function that does not read its parameter. Rejection cases must emit no IR.
The sanitizer runner explicitly marks guest LLVM definitions for ASan and
requires evidence of generated guest memory checks; linking a sanitized C
runtime alone is insufficient.

`Option<PathCapture>` and other class payloads still require an ownership- and
lifetime-aware sum ABI. Borrowed subobject results, such as the router's
`Decision.path()`, must retain the owner's loan; copying or moving a borrowed
owner is not a substitute. The actual Spin server still requires typed HTTP
mailboxes, await frames, request-scoped storage and the remaining route graph.
This milestone does not establish server parity or a performance result.

## Conditional-sum source-to-native parity gate

The existing `native_class_string_parity` Rust executable harness includes
five canonical `.atto` conditional-sum fixtures: two valid Option/Result
tagged-branch guests executed through `oreslang-llvmc`, Clang/native O1,
and the harness sanitizer configuration; and three negative mixed scalar/
tagged conditionals requiring the **exact semantic rejection** and no
emitted LLVM IR. Legacy fixtures remain `.ores`.

These are test definitions, not executed evidence. The exact-head compiler,
native guest stdout/exit, and ASan/UBSan receipts remain required.

## Compiler-created descriptor validation

Native C++ compiler ASTs are not automatically trusted. The sum registry
now rejects duplicated `Option`/`Result` names, a mismatched case kind,
noncanonical payload names, an invalid `ScalarType` or sum kind, and
noncanonical hidden `Option` error metadata before the resolver or scalar
type checker can pass descriptors to LLVM code generation.

The standalone native regression adds eight forged-descriptor mutation
cases, alongside the existing conditional-arm and payload tests. These
are compiler-host tests, **not** evidence of source-defined nominal
`define enum`, recursive ADTs, .atto guest execution, or Native AOT
parity. Exact-head LLVM CTest execution is required.

## Malformed compiler ASTs

The resolver explicitly rejects null sum-constructor payload pointers,
conditional expressions with a missing true/false value arm, and scoped
or iteration statements missing their declaration binding. This prevents
null-pointer dereferences when an internal compiler stage supplies a
malformed AST. Six additional host-only negative mutations raise the
standalone native-sum admission suite to 37 assertion **definitions**.
Re-execute this changed source at the exact PR head before claiming
37 passing assertions; previous host-carrier receipts apply only to
their original byte-identical compiler/test blobs.

## Shared untrusted-AST shape validation

Both `resolve_native_sums` and `validate_native_scalar_types` now invoke the
same `validate_native_sum_expression_shape` preflight before walking AST
children. It rejects null tagged constructor/member arguments, missing
member receivers, incomplete conditional predicates, ambiguous dual
conditional predicates, and missing conditional result arms. The scalar
typechecker's loop and scoped-binding dereferences receive the same
explicit rejection as the sum resolver.

Five additional host mutations expand the regression corpus from 37 to
42 **defined** assertions. These checks need a fresh exact-source test
run; previously successful selected-blob carrier runs do not certify
this new compiler/test source tree or guest `.atto` execution.
