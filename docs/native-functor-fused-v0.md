# Experimental LLVM native functor fused scalar ABI v0

**Status:** host/native C11 ABI prototype only. No AttoLang guest compilation or
full zero-GC certification.

The C header in src/main/cpp/oreslang/runtime/NativeFunctorFusedV0.h is a
bounded 64-slot, actor-scoped, generational handle arena. It does not perform
malloc/free or tracing GC. It does not become part of the runtime until
independent ABI review and typed compiler lowering.

- Option<Result<int,E>> and Result<Option<int>,E> use explicit 3-variant fused tags;
  transpose is an exact, allocation-free, in-place tag rewrite.
- Option<Option<int>> and Result<Result<int,E>,E> flatten exactly one layer.
- Nominal E remains a nonzero error-domain identifier across transforms.
- Immediate-scalar map/map_err run on the active variant only, never on None/Err
  where their callback is dormant; nested implicit mapping is rejected.
- Borrowed values reject transpose, flatten, map and drop. Generation checks
  reject stale copied handles after a destructor releases their slot.
- An owned resource token has exactly one registered destruction callback.
  Scalar map/map_err deliberately reject affine-resource tokens to prevent
  callback-based move/copy laundering.
- Bounded arena exhaustion is deterministic and does not fall back to a heap.

**Local native executions (source SHA matched):** GCC -O0 PASS; GCC -O2 PASS;
Clang -O1 AddressSanitizer + UndefinedBehaviorSanitizer PASS, with strict
warnings as errors and assertions enabled. Source SHA1 Git blob
2c29c6fe9b6c451926e655321e185f0f3c03f253;
test blob e5cd9383e86e87bcef090101b2b65a72b189849b.
These are local host/native C proof steps, **not** LLVM .atto guest steps.

**Explicit limitations:** the C API exposes cell handles and is not a
self-contained secure borrow checker: a future typed LLVM checker must prove
linear transfer and authority, and real actor-domain isolation remains
unverified. This v0 representation is limited to fused scalar nested sums.
It cannot represent general 3-layer ADTs, user-defined records, ownership
of nested arbitrary values, callback captures, or closures. It is an
implementation proof-of-concept, not an ABI that may be merged directly into
production without review.

**Remaining admission gates:** integrate recursive type rules and emitted
compiler IR; execute exact 17 canonical .atto fixtures with O0/O2 guest
machine-code and expected 36-positive/15-negative cases; prove memory
ownership with destructor faults and actual guest sanitizers; independently
prove the native no-GC linked image and FFI closure. Do not promote a
source-free or host-only pass to backend conformance.

No cross-organization source mirror is created. Source-org actions must
execute actual steps to count as independent CI evidence.
