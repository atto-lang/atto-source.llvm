# Product records: mandatory LLVM native AOT and zero-tracing-GC admission gate

Status: **blocking acceptance contract**, not proof that native record spread is implemented.

## Definition of complete AOT

The product-record source must compile to an executable whose record expressions, static call permutation and field loads/stores are emitted as **native guest instructions**; the executable must not ship or invoke a source evaluator, JIT, interpreter fallback, runtime compiler, reflection-based argument binder, generic Map iteration for call binding, or dynamically loaded bytecode interpreter.

A GraalVM native image of the Java interpreter proves only that the **interpreter** was built AOT. It is not evidence that an Oreslang guest program was fully ahead-of-time lowered. These are distinct claims.

## Zero GC is not zero allocation

LLVM strict mode must not link a tracing or moving garbage collector, call `GC_malloc`, create a `gc` function attribute/strategy, emit `gc.statepoint`, `gc.relocate`, or `gc.result`, or fall back to a managed host VM.

- Fixed-size scalar tuples and records with statically known layout should use registers or stack slots when they do not escape.
- Escaping records carrying owned nontrivial values must use an actor-owned native region or explicit deterministic owner storage, with compile-time drop scheduling. A C `malloc`/`free` backend is allowed **only** behind explicit deterministic ownership; it is not a collector, but can still incur allocator latency.
- Each field has independent move/borrow provenance. A spread reads or projects fields once; a move consumes exactly one owned field and leaves it uninitialized. No implicit copying of linear payloads to reorder call parameters.
- Borrowed field pointers, mutable loans, FFI and async continuations must not outlive the aggregate owner or escape the actor memory domain. Suspend/cancel/panic/raise/early-return paths must release loans and drop moved temporaries exactly once.
- Heap cycles require an explicit non-owning edge, checked handle, or reviewed region policy; no hidden reference-count cycle leak or tracing-collector escape hatch.
- Shared actors receive frozen values; other actors receive transferred/detached message snapshots. A record spread is not a backdoor for shared mutable addresses.

## Executable verification matrix required before support claim

1. Compile actual guest source for both `foo(...(x, y))` and `foo(...(b: y, a: x))`. Assert the same ABI slots and output; use side-effectful initializers to assert lexical evaluation order once.
2. Exercise compile-time failures: runtime map/unbounded record spread, unknown/duplicate/missing named parameter, borrowed/move-only alias duplication, mutable+immutable overlapping loans, returned loan to stack product, cross-actor pointer, loan alive at await/select, and destructors skipped by cancellation/throw/raise/panic.
3. Use runtime counters to require **allocations = frees** for all dynamically owned native records after quiescence; baseline live bytes before and after repeated production workloads must match. Include failpoints at each allocation and cleanup boundary, thread sanitizer/ASan/UBSan, and multiple carrier migration.
4. Inspect **linked** native executable and emitted IR: no tracing GC API symbol, GC intrinsic/statepoint or GC strategy, no guest parser/compiler fallback, no runtime reflection-based binder; compile with optimizations at `-O0` and `-O2`. A `grep` of one scalar function is not sufficient coverage.
5. Assert native executable works without Java, Maven, or a source compiler on PATH. For strict fully compiled mode, test against a packaged guest artifact without reading its original `.ores` file at runtime.
6. Compare LLVM guest behavior and diagnostics to Java (JVM, native AOT and hybrid), keeping physical actor execution single-flight. Evidence requires exact Git tree identity and independent CI results.
7. Guard against `LLVM_NOGC=1` or similar switches merely suppressing collector calls while leaving heap owners dangling; use dynamic lifetime/leak checks and the ownership-CFG report.

## Current honest boundary

The LLVM repository documents native scalar and actor-owned i64 heap subsets, and deterministic heap/loan substrates, but these do **not** establish general compiled tuple/record spread, native class/struct lowering parity, or full no-GC application support. Until the executable matrix above passes, record spread stays **compile-time rejected**, not partially interpreted.
