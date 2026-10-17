# Experimental Linux eBPF / XDP target (stage 1)

**Not production eBPF support.** This initial LLVM target is designed to
establish a trustworthy `.ores → verified LLVM IR → real EM_BPF object` path.
It does **not** establish Linux verifier acceptance or attach an XDP program.

## Admitted Oreslang source

Constant-action programs remain supported:

```oreslang
pub fnc ores_xdp(): int {
    return 2; // XDP_PASS
}
```

Stage 1 also admits an **opaque, shared borrowed** kernel context with one
verifier-aware byte accessor:

```oreslang
pub fnc ores_xdp(borrow XdpContext ctx): int {
    if (ctx.packet_u8(14) == 8) { return 1; } fi // XDP_DROP
    return 2;                                    // XDP_PASS
}
```

`XdpContext` is compiler-owned and target-scoped. It is not constructible,
storable, movable, mutable-borrowable or available to ordinary host/native
Oreslang. The source never receives raw `data`, `data_end`, or a packet
pointer. For every `packet_u8(const_offset)`, the BPF lowerer emits the
kernel-context loads and proves `data + offset + 1 <= data_end` before the
byte load; an out-of-bounds path falls through without dereferencing.

Build the compiler, then:

```sh
./build/oreslang-llvmc --target=bpfel --bpf-program=xdp examples/xdp_pass.ores > /tmp/xdp.ll
clang --target=bpfel -O2 -x ir -c /tmp/xdp.ll -o /tmp/xdp.o
readelf -h /tmp/xdp.o     # ELF64, little endian, EM_BPF (247)
readelf -W -S /tmp/xdp.o  # xdp, license sections
```

The accepted form must contain one **exported**, top-level `ores_xdp`.
It may have no parameters, or exactly one `borrow XdpContext <name>`
parameter for the bounded `packet_u8` guard slice. Return actions remain
literal basic XDP actions:
0=XDP_ABORTED, 1=XDP_DROP, 2=XDP_PASS, 3=XDP_TX.
`XDP_REDIRECT` is intentionally not admitted: redirect requires state prepared
by `bpf_redirect()` or `bpf_redirect_map()`, which stage 0 does not expose.
The compiler emits the kernel entry ABI `i32(ptr ctx)`, marks section
`xdp`, and emits a `Proprietary\0` license section. This is deliberately a
**non-GPL kernel capability marker**, not a legal source-license declaration:
stage 0 has no helpers, and later GPL-only helper/kfunc support must require an
explicit licensing policy rather than silently inheriting compiler defaults.
It never exposes raw kernel pointers to Oreslang source.

The target rejects source modules, additional functions, user classes,
actors, generics, locals, general statements/calls/arithmetic, suspension,
runtime effects, mutable context borrows, raw pointers, maps/helpers,
recursive code and unsupported packet operations. The only borrowed object
currently admitted is the target-scoped `XdpContext`, and only
`packet_u8(literal_offset)` equality guards are accepted. Source size is capped at 4096 bytes before parsing. There is no
host main wrapper or host runtime linkage. No automatic fallback occurs.

## Current focused evidence (2026-10-17)

Private exact-blob run
[39076669825](https://github.com/ores-dummy-org-oreslang-stack/ores-dummy-org-oreslang-stack-lambdas/actions/runs/39076669825)
passed the focused exact-blob compiler/XDP/ABI suite and a real
Linux 6.17.0-1022-azure verifier/`BPF_PROG_RUN` gate. Constant XDP actions
0..3 passed at both O0 and O2. The bounded packet program was verifier-accepted
and exercised four runtime paths: exact `data_end` boundary fallback,
in-bounds mismatch fallback, in-bounds byte match, and a normal 1500-byte
packet fallback. Negative verifier fixtures also rejected uninitialized R0,
invalid context access, out-of-bounds stack access, and an unknown helper.

This is **focused exact-blob evidence, not full repository tree evidence**.
It does not yet prove source-tree-wide CTest, multiple kernel versions, maps,
helpers, BTF/CO-RE, live attachment, or network namespace behavior.

## Evidence boundary

The dedicated CTest checks positive XDP actions, negative source admission,
actual Clang BPF object generation, ELF architecture, and required sections.
It requires a Clang distribution with the BPF backend and binutils readelf.
A successful IR verifier result alone is **not** sufficient evidence.

Remaining milestones, in order:
1. Run an **exact full-tree** build and complete CTest inventory; the current
   passing evidence is a focused 20-blob compiler/XDP slice.
2. Repeat verifier/test-run evidence across an explicit supported Linux
   kernel matrix, and separately test live attach/detach in an isolated
   network namespace with deterministic cleanup.
3. Expand the proven packet API beyond `packet_u8(literal)` only after
   adding width/alignment/endian contracts and corresponding verifier fixtures.
4. Add map definitions, helper/kfunc gating, explicit source/deployment
   licensing policy, BTF and CO-RE only with versioned kernel ABI contracts.
5. Expand the negative verifier corpus beyond uninitialized-register proof,
   including bad context bounds, illegal helper capability and map lifetime.
6. Expand to socket/TC/tracing independently after program-type-specific
   admission and verifier proofs; keep `bpfel` itself a generic target.

**AF_XDP is a user-space packet I/O facility and does not itself certify
eBPF code.** BPF is a kernel execution target, not an actor scheduler:
source actors, GC, async/await and arbitrary imports remain excluded.
