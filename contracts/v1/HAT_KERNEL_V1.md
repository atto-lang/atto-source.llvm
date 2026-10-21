# Oreslang HAT kernel and stdlib contract v1

Status: parser/IR contract; executable accelerator lowering is not enabled by this document.

## Canonical kernel syntax

```oreslang
define pub kernel VectorAdd as
  fnc run(int index): void {
    return;
  }
end
```

`kernel` is a reserved word. Canonical declarations use
`define [pub|private] kernel <Name> as ... end`.

A kernel is a backend-neutral heterogeneous-compute declaration. Its body is
currently restricted to synchronous `fnc` declarations. Actor messaging,
normal actor state access, suspension, I/O, and implicit host fallback are not
part of the kernel contract.

## Safety boundary

Parser/IR admission is not executable admission. Before a kernel may execute,
the compiler must prove:

- device-visible reads/writes obey Oreslang ownership and borrow rules;
- a host or actor owner cannot mutate memory while a conflicting device loan is live;
- device loans remain live through the completion fence, not merely submission;
- actor isolation is preserved and no actor critical state loan is held across device execution;
- every operation is supported by the selected backend capability set;
- unsupported kernel operations fail closed rather than silently running on the CPU.

The Java/Graal frontend therefore represents `KernelDecl` but rejects normal
type/executable admission until HAT effect/ownership lowering exists. The
constrained LLVM frontend reserves the syntax and rejects it explicitly at its
native admission boundary until parity lowering is implemented.

## Standard library

The canonical portable API is `std/hat`. The standard library owns stable
semantics and contracts for devices, capability sets, queues, fences, host and
device buffers/views, kernel handles, dispatch ranges, and backend providers.

Vendor-specific backends are not stdlib implementations. CUDA, ROCm/HIP, Metal,
Vulkan, OpenCL, NPU, and future accelerator providers remain pluggable/userland
backends implementing the HAT contracts.

The standard library must not grant a backend authority to bypass compiler
ownership, effect, actor-isolation, or completion-lifetime checks.

## Execution model

Actors may own or coordinate device/queue handles, but GPU lanes/work-items are
not actors. Submission may outlive the actor turn that initiated it; therefore
resource state should be captured as a versioned device lease and the actor
state loan released before waiting for device completion.

CPU/reference execution is the conformance oracle. Accelerator backends must be
differentially testable against it.
