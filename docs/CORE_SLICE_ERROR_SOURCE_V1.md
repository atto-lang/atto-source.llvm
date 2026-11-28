# LLVM freestanding core/slice and core/error source candidates

These are **independent LLVM repository-owned .atto files**, not a shared-source
dependency or a claim that native LLVM currently compiles/imports/executes them.
The source is kept under `library/ores.core/` alongside `collections.atto`.

- `slice.atto`: signed negative-index normalization, explicit half-open bounds,
  range length, overflow-free contiguous/positive-stride view preflight.
  Invalid indices/bounds return -1; validation returns bool. These do not
  grant memory-access, aliasing, owner or lifetime permissions.
- `error.atto`: versioned built-in error-code validation, bounds/retry/actor
  API classifiers and message-size preflight, without creating or spoofing
  a nominal Error class. Numbers never prove class identity or authorize
  throw/raise/panic, actor CONTROL or retries.

**Still unsupported:** actual `Slice<T>`, `StridedSlice<T>`, mutable slice
borrows, compiler-checked owner provenance, negative-stride views, effect-safe
Error values/cause/trace, automatic code-generation, and native guest execution.

Run native import/linker/IR and ownership-negative tests before claiming
support, including MIN_INT/MAX_INT, empty slices, explicit-vs-implicit
negative end, one-past-end zero length, reverse stride, crossing await,
actors and use-after-free. Reconcile existing LLVM Error PRs #327/#328,
Atto spec PR #31, and Java paired source PR #1768. Source-only parity is
not runtime/compiler parity.

## Follow-up milestone (2026-11-22)

The independent source is now extended to match the *public semantic
contracts* of the Java companion PR #1768:

- `valid_range(offset,count,length)` is a logical window, never a pair of
  endpoints. `valid_endpoints(start,end,length)` has distinct semantics.
- `get_copy_int`, `first_copy_int`, `last_copy_int` and
  `subslice_offset` return `Option<int>` and do not expose pointer-backed
  values. **Native guest support for Array<int>.get_copy is not verified.**
- `valid_reverse_window` preflights negative strides including MIN_INT, but
  does not create reverse views or grant borrow permissions.
- `error.code_group` maps known catalog codes to *numeric* groups only.
  This must never be used for trusted nominal type or catch admission.

The proposed `tests/fixtures/core-slice-error-v2/main.atto` is deliberately
**unadmitted** until native import resolution and guest O0/O2 execution pass.
No synthesized success or source-free CI outcome counts as implementation.

The sibling LLVM PR #872 introduces `core/slice.atto` with integer-array
algorithms and `valid_range(offset,count,length)`; reconcile its algorithm
contents rather than creating competing semantics or requiring shared source
bytes between backends.

## Raw vs normalized indexing

`valid_index(i,n)` checks only raw nonnegative indices in `[0,n)`;
`valid_signed_index(i,n)` accepts `[-n,n)` and applies negative
normalization as in the candidate collection spec. `get_copy_int` uses
`normalize_index`, so negative relative indexes still return owned copies.
`is_empty(count)` and `can_split_at(count,middle)` follow the sibling
slice/iter/cmp logical-window API. Never conflate `valid_range` with
`valid_endpoints` or raw with signed indexing.

## Ownership correction — explicit borrowing

`get_copy_int`, `first_copy_int` and `last_copy_int` now accept
`borrow Array<int>`, not an owned `Array<int>`. Call with
`rt borrow values`: passing an owned array by value consumes its unique
owner under Atto's affine parameter rules. The checked-copy helpers must
never move the owner merely to return `Option<int>` or `None`. First/last
read directly from a validated borrowed array, avoiding an implicit reborrow
or move through an intermediate helper. Access under an outstanding exclusive
mutable loan must be rejected, even on out-of-bounds paths. Native LLVM must
implement these semantics before the unadmitted guest fixture can pass.
