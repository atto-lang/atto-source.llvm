# AttoLang explicit return control-flow contract — LLVM

All non-`void` functions and methods must contain explicit source
`return value;` statements covering every reachable exit. No implicit
result from the final statement. A `void` function may fall through
at its closing brace or explicitly `return;`. Returning a value from
a void function is a semantic error; `return;` without a value is
invalid for non-void results.

The only implicit value return in the language is a **braceless
expression-bodied lambda**, e.g. `|x| -> x + 1`. A lambda with `{ }`
is statement-bodied and needs an explicit `return` if its result type
is nonvoid. A nested lambda's return does not discharge the enclosing
function's obligation. Lambda compilation is **not** promoted/certified
by this LLVM scalar source patch; future lambda support must implement
that distinction explicitly rather than relaxing the function parser.

The native scalar compiler previously unconditionally required a
trailing `return` even in a void body; it now permits an absent
terminal return **only when the declared return type is void**.
The LLVM backend already emits `ret void` when `returns_void` is
true; a real compiled guest must verify that fact. The nonvoid
path fails at source admission with `RETURN_EXPLICIT_REQUIRED`,
and separate diagnostics identify missing values and invalid void
values. The ordinary parser is unchanged outside these return
boundaries; it does not infer a result for a nonvoid conditional/ADT
match.

Run actual compiler-backed positive .atto programs and semantic-negative
inputs at the exact PR head. Private funded mirrors are authentic
allowlisted Git blob subsets, not full original Git trees. Independent
review remains required before promotion.
