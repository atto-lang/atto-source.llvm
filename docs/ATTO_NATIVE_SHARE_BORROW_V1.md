# Native AttoLang: borrowed parameter cannot be promoted by `rt share`

**Draft: source authored, compiler/guest tests NOT_EXECUTED.**

## Compiler ownership finding

The native source checker previously allowed `rt share borrowed` when
`borrowed` was a named `borrow int` parameter. It checked that the
parameter was not a move-only class/array, then ran the scalar value-read
checker. That is not an independent ownership or sharing authorization:
an incoming loan has no rights to transfer the source owner's authority.

The `Share` checker now rejects `source.mode != Mode::Value` with
`E_SHARE_BORROW` before reading its value. The compiler keeps the existing
scalar `rt share 42` positive case, the class/array `E_SHARE_MOVE_ONLY`
negatives, and ordinary borrowed scalar argument reads. Callers wishing to
share a scalar must first produce a separately **owned**, proven Copy value.

## Regression and proof requirements

- `tests/fixtures/atto-share-borrowed-parameter-reject.atto` contains a
  `borrow int` formal and an attempted `rt share incoming`.
- CMake registers `atto_share_borrowed_parameter_rejected` as a source
  compiler-negative CTest with the specific ownership diagnostic.
- Exact-head native CI now includes **19** scoped compiler/guest CTests,
  plus the independently registered host CFG-join and source-place CTests.
  A parse failure is **BLOCKED**, not borrow safety; compiler outputs
  must show parsing succeeded and the ownership checker issued the error.
- Until a nonzero-step source-owned runner executes the production compiler,
  neither parser acceptance, CTest execution nor guest parity is established.
  No external/private-source CI mirror is authorized.

This only closes the direct named loan upgrading path in the restricted
LLVM scalar frontend; general `Share` type traits, actor/tenant domains,
mutable sharing and MIR nonlexical lifetimes still need independent proof.
