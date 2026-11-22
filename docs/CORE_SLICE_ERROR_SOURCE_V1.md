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
