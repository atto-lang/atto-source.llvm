# DOP inverted storage contract v1 (candidate, not five-backend parity)

These canonical `.atto` fixtures are **byte-identical** in the source-local
LLVM and Java repositories. They define a restricted, observable storage trace,
not a complete general-purpose inverted-class API.

- `swap-reuse.atto`: two local two-i64 rows, swap-delete dense row 0,
  insert a fresh row into freed capacity, read dense row 1; expected native
  process exit **42**.
- `removed-index-trap.atto`: after two inserts and a swap-delete the
  dense length is 1; attempting read of dense index 1 must terminate with
  a real bounds signal (not a forged exit code) on the narrow LLVM path.

**Distinct concepts:** row 1 is a dense index, *not* an opaque, owner-bound,
generation-fenced row handle. Reusing a formerly valid dense index does not
model the semantics of stale `RowHandle` objects. Stable handles, foreign
owners, disjoint field loans, AoSoA tiles, fixed records, general inverted
classes, actors, and non-i64 fields require separate source and ABI fixtures.

## Evidence and qualification

LLVM #1403 should compile both through actual `oreslang-llvmc`, screen all
generated IR with the source-built no-GC auditor, link/run under Clang O0/O2,
and check the return code or *real* signal. Java #2550 currently supplies
**host-only** SoA backing and test support; it does not wire these sources
to its parser/evaluator. Java execution must remain NOT_EXECUTED until a
separate independently reviewed language-admission integration exists.

No host-only test, static fixture identity or source-free CI can be counted
as cross-backend executable AttoLang parity. Keep public guest composition
in canonical `.atto`, not a new Java/C++ business-logic copy.
