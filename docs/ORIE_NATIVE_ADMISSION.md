# Native ORIE compiler admission — staged source change

This source-local patch is a **draft**, not a native execution or `f64`
parity certificate. ORIE's [native feature gate](https://github.com/ores-truffle-oreslang/oreslang-orie-ieor/pull/1)
requires six native guest capabilities followed by 13 application fixtures.

## Implemented in this patch

The native expression parser recognizes the **canonical Oreslang console**
`stdio.stdout.print(...)` and `stdio.stdout.println(...)`, while
preserving existing `stdio.print(...)` and `stdio.println(...)` guest
source. Both syntaxes map to the **same checked Console AST**, including
left-to-right argument evaluation, string concatenation, native
nonblocking stdout reactor, and ownership/effect checks.

The alias does not introduce new streams or a fallback to blocking
`printf`, Java, or the host script. `stdio.stderr`,
`stdio.stdout.write`, and other unverified surface forms remain rejected.

`tests/native_orie_stdout_alias.sh`, registered as
`native_orie_stdout_alias` in CTest, requires real
`oreslang-llvmc --target=host`, LLVM's normal
`scripts/compile-aot.sh` (including runtime C sources if needed),
native guest exit 0, and exact piped output for three positive
programs. Three unsupported stream spellings must fail compilation
with no emitted IR.

## First f64 lexer stage (NOT native lowering)

The lexer now distinguishes a complete decimal/scientific numeric token
from an integer token followed by a dot or exponent identifier. This
prevents `0.3` from appearing as a partial i64 expression and `1e2`
from appearing as integer `1`. A lexical malformed exponent such as
`1e+` fails explicitly. Complete floating literals are then rejected
with `native f64 literal requires typed IEEE-754 lowering; unsupported`.
No `f64` AST/type identity, conversion rule, comparison, arithmetic or
LLVM `double` lowering has been claimed or implemented.

`tests/native_f64_lexical_gate.sh` has one **actual executable i64
positive control** (must exit 42) and five f64 source-negative cases
that must return nonzero and produce **no LLVM IR**. The exact
compiler-backed test is registered as `native_f64_lexical_fail_closed`
in CTest. A passing test is **negative admission evidence**, not an
implemented floating-point feature.

## Next upstream milestones

1. Add distinct `f64` AST, lexical literal storage and explicit type
   checking. Enforce the language's **no implicit conversions** rule.
2. Lower correctly typed finite f64 arithmetic and ordered comparisons
   to LLVM IR using IEEE semantics. Check NaN/infinity/signed-zero,
   rounding, unsupported operations and exact-return ABI.
3. Run positive/negative actual guest programs on a trusted runner with
   exact source SHA/tree, compiler binary digest, executed CTest jobs,
   IR/native artifact hashes, exit codes and diagnostics.
4. Extend owned `Array<f64>` representation and lifetime checking
   separately; do not pretend `Array<int>` ABI proves float arrays.
5. Run ORIE's feature matrix (6/6), then its 13 Oreslang application
   fixtures, followed by independent numerical oracles.

Tracked at [LLVM issue #453](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/issues/453)
and [LLVM issue #428](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/issues/428).

**No compiled guest pass claimed:** the source organization's GitHub Actions
are currently failing before execution (`steps=[]`). Keep this PR draft,
and do not mirror private source outside the authorized security boundary.
