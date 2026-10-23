# LLVM ORC JIT: first executable source slice

This is the first **opt-in host execution path**, not a claim that LLVM has
Truffle-style profiling or that Oreslang's Java and LLVM backends have reached
behavioral parity.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --target oreslang-llvmc --parallel 2
./build/oreslang-llvmc --jit tests/fixtures/forward-call-main.ores
echo $?   # 42
ctest --test-dir build -R '^native_orc_jit_compiled_source$' --output-on-failure
```

## Architecture

1. Use the **same** source parser, name/arity/ownership admission and LLVM IR
   codegen as the existing Clang AOT path. Never implement a second, looser
   source grammar just for the JIT.
2. The existing codegen verifies the LLVM module. The ORC path reparses that
   emitted IR into a private LLVMContext and verifies it again.
3. Admit only a generated, defined `i32 main()` native process wrapper; this
   retains the compiler's documented guest `int` to OS-exit-code conversion,
   including rejection/status conversion of values outside `0..255`.
4. Reject imported functions, unresolved externals and external globals.
   Do not install LLVM's unrestricted current-process symbol resolver.
   Host library bindings require a future explicit allowlisted capability ABI.
5. Load the module into LLVM ORC **LLJIT** using ThreadSafeModule, resolve the
   unmangled source-visible native wrapper via LLJIT, execute it, and return
   its status. With `--jit`, stdout is guest output, **never LLVM IR**.
6. This is a **trusted developer tool**, not a sandbox. Compiled native
   machine code runs in the compiler process. Never admit untrusted actor
   code into this shared address space or conflate ORC JIT with the GraalVM
   pooled untrusted isolate.

`--jit` deliberately rejects wasm32, Cortex-M, experimental owned-int or
scalar-loop modes, hard-real-time reporting/check modes, and source programs
without a native entry wrapper. Standard IR output and Clang AOT are unaffected.

The source/ORC CTest gate runs seven positive source programs twice each
(exercising direct calls, forward calls, nested calls and main exit-code policy),
compares every one against actual Clang-linked AOT execution, checks a
tree-shaken source program, and runs eight fail-closed negatives. This gate
tests **real JIT execution**, rather than checking emitted LLVM IR alone.
It does **not** test equivalent execution by Java/GraalVM. The complete
conformance ledger remains open.

## Parity milestones after the initial JIT runner

| Milestone | Requirement |
| --- | --- |
| Typed IR | Single validated typed lowering for String, float, ADTs, classes, errors, function effects; no guessing on dynamic types |
| Shared observable semantics | Same Java/LLVM source fixtures with typed outcomes, diagnostics and lifecycle traces |
| Runtime ABI | Explicit native intrinsics and capability-checked symbol bindings; continuation and actor runtime ownership |
| Hot reload | Versioned function/actor code images, atomic generation swap, call/continuation pinning, rollback, code retirement receipt |
| Tiered JIT | Collect low-overhead execution counters, determine compilation threshold, background compile safely, cache machine code; test deterministic AOT fallback |
| Untrusted JIT | Strong separate process/isolate memory boundary, bounded resources and termination; never claim that LLJIT itself supplies one |

LLJIT eagerly compiles required functions on lookup. LLVM's LLLazyJIT,
profiling-guided recompilation, or MLIR dialects are optional implementation
directions, not features delivered by this slice. MLIR should be introduced
only when its typed dialect and lowering tests reduce real compiler complexity;
the existing AST-to-IR lowering is already a valid production foundation.

**Release gate:** native CTest + fresh Java/GraalVM differential execution and
actor/ownership/supervision source-level safety evidence. Parser acceptance,
standalone C++ host tests and JIT smoke tests alone are insufficient.
