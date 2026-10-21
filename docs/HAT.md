# Oreslang HAT native boundary

Status: **v0 reserved syntax; native kernel execution is fail-closed**.

`kernel` is a reserved Oreslang keyword shared with the Java/Graal frontend.
The canonical declaration shape is:

```oreslang
define pub kernel K as
  // future typed kernel body
end
```

The LLVM frontend currently rejects this boundary with a dedicated diagnostic.
It must not reinterpret a kernel as an ordinary host function or silently fall
back to CPU execution.

The portable HAT contract is defined by Oreslang core/std-lib policy. Native
lowering may be enabled only after the compiler has a versioned typed kernel IR,
effect and ownership admission, explicit host/device transfer rules, and
completion-fence lifetime tracking.

Vendor implementations such as CUDA, ROCm/HIP, Metal, Vulkan and OpenCL remain
pluggable providers rather than compiler syntax or hard-coded standard-library
implementations.

Native enablement order:

1. parse a real kernel AST;
2. emit/version portable kernel IR;
3. add CPU reference execution for differential checking;
4. add opaque device buffers/views and fence-backed borrow lifetimes;
5. add async queue/dispatch and explicit transfers;
6. bind one provider backend only after capability/layout validation.
