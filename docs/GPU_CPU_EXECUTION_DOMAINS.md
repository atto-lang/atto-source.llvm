# CPU and GPU execution-domain contract (proposal, v0 safety gate)

Status: **reserved and fail-closed on Java/Graal and LLVM main-derived hardening branches**.
This is a source-language and compiler boundary contract, **not** a claim that
nested domain scopes, device launches, CUDA, OpenCL dispatch, or data migration
are implemented. `gpu`, `cpu`, and `quantum` are reserved keywords.
Presently source containing these tokens fails at compile/parse time. Existing
independent DOP CPU kernels, OpenCL source emission, GPU fence ledgers, and draft
accelerator PRs do not by themselves authorize ordinary source execution.

## Intended syntax

```oreslang
// FUTURE SYNTAX ONLY — intentionally rejected by today's compiler.
suspend fnc pipeline(HostBuffer input): HostSummary {
    let device = await gpu {
        let device_input = await Gpu.upload(rt take input);
        launch_transform(rt take device_input);
    };
    return await cpu {
        let host = await Gpu.download(rt take device);
        summarize(host);
    };
}
```

Both `cpu { ... }` and `gpu { ... }` are expression-capable *execution
regions* with a typed value/result and effect signature. They are not placement
hints, lexical macro substitution, or authorization to execute guest code on
an arbitrary scheduler thread. A domain transition is an asynchronous,
cancellable operation in the runtime execution graph.

## Nested-domain lowering

| Nesting | Required lowering | Forbidden shortcut |
| --- | --- | --- |
| CPU → CPU | Host lexical region; compiler may fold if effects match | Double-publish mutable borrows |
| CPU → GPU | Enqueue validated device launch; await or carry typed future | CPU-evaluate kernel on missing device |
| GPU → GPU | Separate ordered launch, or proven-safe fuse on supported target | Device-side arbitrary kernel launch |
| GPU → CPU | End/split device kernel, satisfy fence, schedule host continuation | Call host code directly from GPU threads |
| GPU → CPU → GPU | Device completion → explicit download/transfer → host continuation → explicit upload/dispatch | Hidden migration, reentrant device callback |

A `cpu` region *nested syntactically* within a `gpu` region is a host
continuation in the graph. A GPU kernel body is still restricted to device-safe
operations. Compositional nesting does not mean arbitrary CPU/GPU recursion or
hardware dynamic parallelism. The scheduler must never synchronously wait on
the device on an actor carrier thread.

## Required compiler safety checks before lifting the v0 rejection

1. Typed `ExecutionDomain` effects: `Host`, `Device(device capability)`,
   `Transfer`, and `Await`. Device kernel admission rejects I/O, heap/host
   pointers, locks, actor mailbox mutation, unwinding/panic, and unsupported
   recursion/dynamic allocation. Domain transitions must be explicit in IR.
2. Typed, bounded device buffer handles: tenant, device, allocation generation,
   byte length, alignment, element layout, and ownership. No host pointer
   masquerades as device memory. No shared mutable host/device borrow while a
   command is in flight. No use after move, drop, cancellation or device reset.
3. Explicit `upload`, `download`, and device/device transfer. A compiler must
   not infer an implicit transfer or fallback solely because a value appears in
   another domain. `GpuBuffer<T>` and `HostBuffer<T>` are distinct types.
4. Ordered futures and completion fences for dependencies. Cancellation is a
   *request*, not proof that a GPU command has stopped. Memory reclamation and
   actor shutdown wait for verified device quiescence; stale completion tickets
   must never revive a retired allocation.
5. Validate target `arch`, 32/64-bit address width, endian/layout ABI,
   alignment and legal workgroup/buffer limits. Never assume a 64-bit CPU
   implies a 64-bit device. OpenCL/CUDA/Vulkan capabilities are checked before
   creating a command; unknown capabilities reject deterministically.
6. Bound per-tenant queue depth, bytes, outstanding transfers, kernel runtime,
   and concurrency. Report device loss, timeout, and unavailable backend as
   typed errors; do not retry mutations or fall back to CPU implicitly.
7. Prove each actor's host continuation executes on at most one carrier at
   once, including completion/cancellation races, without blocking a scheduler
   worker. CPU and device execution must not share unprotected actor state.
8. Formatter retains explicit domain boundaries, and Java + LLVM conform to
   identical parse/type/effect/IR fixtures for legal and rejected programs.

## Today: compilation checks

The Java parser reserves all three domain tokens and rejects their presence
before parsing the program, including unreachable code, nested blocks,
declarations, and attempts to use them as ordinary identifiers.

The native parser also reserves `cpu` alongside `gpu` and `quantum`.
Unsupported domain syntax rejects without emitting LLVM IR. Existing native
GPU/quantum diagnostic prefixes are preserved for compatibility.

When implementation begins, lift the front-end gate **only** for AST nodes
that have complete typed effect checks, ownership admission, and verified
backend lowering. No broad lexer rollback to identifiers.
