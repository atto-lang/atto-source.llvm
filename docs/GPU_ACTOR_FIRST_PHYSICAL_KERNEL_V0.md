# GPU actor P1 first physical kernel prerequisite

**Status: G1 kernel-only prerequisite. It cannot certify GPU actor execution.**

This builds on `tests/atto_gpu_physical_saxpy_probe.cpp` (existing hardware-required native OpenCL probe) with `tests/fixtures/gpu_actor_saxpy_f32_v0.cl` and `tests/gpu_actor_saxpy_hardware_gate.sh`. The device kernel has a checked logical tail guard for 1003 values rounded to 1024 work-items. The host probe uploads device buffers, submits OpenCL work, observes a provider-reported completion event through OpenCL (not an independently attested hardware event), downloads output, compares against an independent f32 CPU oracle, and reports device model, vendor and driver. It does NOT compile AttoLang guest source and is NOT an actor scheduler.

## Run on a Linux host with OpenCL 1.2 and an actual GPU

```bash
c++ -std=c++17 -O2 -Wall -Wextra -Werror tests/atto_gpu_physical_saxpy_probe.cpp \
  -o /tmp/atto_gpu_physical_saxpy_probe -ldl
bash tests/gpu_actor_saxpy_hardware_gate.sh \
  /tmp/atto_gpu_physical_saxpy_probe tests/fixtures/gpu_actor_saxpy_f32_v0.cl
```

CPU-only OpenCL devices, nonqualifying GPUs, missing ICDs, build errors and event failures must exit nonzero. No hardware test should be marked skipped-success or silently fall back to CPU. A successful probe is native kernel evidence only; independent GPU hardware provenance and compiler guest evidence are still required. A failed or unavailable GPU is a genuine blocker, not a regression in the compiler.

## Integrate with the AttoLang actor runtime next

The future `gpu actor` runtime must own a bounded mailbox, actor generation, sealed state and explicit `GpuBuffer<f32>` loans; submit work through the authenticated portable compute provider using existing `GpuBufferFence` and `PortableComputeAuthority` components; await an event without parking the scheduler; publish results only after a provider-authenticated fence; reject stale completions; send kernel errors/device-loss failures to the supervisor CONTROL path. Actor identity must never be inferred from a device kernel invocation ID.

Do not enable `gpu actor` compilation on the strength of this fixture. See Java GPU actor draft #2760, LLVM fail-closed #1645, common source HIR #260 and conformance #663.
