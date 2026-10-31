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

## First typed native f64 lexical scalar slice (source implementation; UNEXECUTED)

The lexer distinguishes complete decimal and scientific f64 tokens. The
native AST now stores the separately typed binary64 value as a `double`
rather than inventing an i64 representation. `std::from_chars` performs a
locale-independent conversion: malformed or out-of-range literals fail
before LLVM IR emission; implicit coercion between int and f64 is forbidden.

The first supported scope is **finite f64 literals, explicit typed lexical
locals, unary negation, addition, subtraction, multiplication, f64-only division,
equality and ordered comparisons**. The scalar parser's type pass and independent
`NativeScalarTypes` checker agree on source type identity. The ownership
checker admits f64 literals/pure comparisons but forbids using a float as
an int-only caught-throw payload.

Codegen lowers these values as actual LLVM `double`, with f64 stack slots,
`fadd`, `fsub`, `fmul`, `fdiv`, `fneg`, and `fcmp` operations rather than
bit-casting them to the signed-i64 ABI. Comparisons produce a canonical i64
bool. Ordered relational predicates are false on NaN; inequality uses
IEEE unordered-not-equal. No fast-math assumptions are introduced by this
patch.

`tests/native_f64_lexical_gate.sh`, now registered in CTest as
`native_f64_scalar_e2e`, requires **eight actually compiled/linked/executed
guest programs** (seven f64 checks and one signed-i64 control) to terminate
with exit 42 and empty output. Twelve deliberately invalid source programs
must fail with no LLVM IR, including int-to-f64 coercion, mixed arithmetic,
mixed comparison, integer or mixed-type division, unsupported function f64
ABIs, branch-local f64 owners, malformed exponent and overflowed literals.
These cases are **authored but not executed on an authorized LLVM runner**:
no native f64 PASS, compiler parity or numerical correctness receipt
can yet be claimed.

### Explicit exclusions

This is not a general f64 ABI: floating-point function parameters/returns, branch-local double lifetime slots,
class fields, f64 sum payloads and owned `Array<f64>` need separate
type/ownership/data-layout integration. Floating-point function division is only admitted for f64/f64 expressions
in lexical code; f32 and typed f64 function interfaces are not yet lowered.
int/int division remains explicitly fail-closed.
Exhaustive IEEE bit-vector compatibility, denormals, FP exception behavior,
NaN payloads, signed zero and optimization-level differential tests remain
independent acceptance gates. Numeric routines in ORIE need more than this
scalar slice.

## Next upstream milestones

1. **Execute the current native f64 scalar CTest** against an authorized,
   pinned LLVM compiler and correct any real parser/type/codegen errors.
   Then run IEEE differential cases at multiple optimization levels.
2. Extend the floating ABI to explicitly typed parameters/returns and
   independently define the division, NaN/infinity and FP exception rules.
3. Implement checked owned `Array<f64>` storage, borrows, lifetime and
   element ABI. Existing `Array<int>` runtime is not a proof of this.
4. Require exact source SHA/tree, compiler binary digest, executed CI
   steps, IR/native artifact hashes, exit codes and diagnostics.
5. Run ORIE's six-feature matrix and 13 real application guests, then
   independent numerical oracles and branch protection/review gates.

Tracked at [LLVM issue #453](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/issues/453)
and [LLVM issue #428](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/issues/428).

**No compiled guest pass claimed:** the source organization's GitHub Actions
are currently failing before execution (`steps=[]`). Keep this PR draft,
and do not mirror private source outside the authorized security boundary.
