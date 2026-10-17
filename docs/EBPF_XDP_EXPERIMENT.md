# Experimental Linux eBPF / XDP target (stage 0)

**Not production eBPF support.** This initial LLVM target is designed to
establish a trustworthy `.ores → verified LLVM IR → real EM_BPF object` path.
It does **not** establish Linux verifier acceptance or attach an XDP program.

## Admitted Oreslang source (only)

```oreslang
pub fnc ores_xdp(): int {
    return 2; // XDP_PASS
}
```

Build the compiler, then:

```sh
./build/oreslang-llvmc --target=bpfel --bpf-program=xdp examples/xdp_pass.ores > /tmp/xdp.ll
clang --target=bpfel -O2 -x ir -c /tmp/xdp.ll -o /tmp/xdp.o
readelf -h /tmp/xdp.o     # ELF64, little endian, EM_BPF (247)
readelf -W -S /tmp/xdp.o  # xdp, license sections
```

The accepted form must contain one **exported**, top-level,
zero-parameter function named `ores_xdp` with a literal integer return:
0=XDP_ABORTED, 1=XDP_DROP, 2=XDP_PASS, 3=XDP_TX, 4=XDP_REDIRECT.
The compiler emits the kernel entry ABI `i32(ptr ctx)`, marks section
`xdp`, and emits a `Proprietary\0` license section. This is deliberately a
**non-GPL kernel capability marker**, not a legal source-license declaration:
stage 0 has no helpers, and later GPL-only helper/kfunc support must require an
explicit licensing policy rather than silently inheriting compiler defaults.
It never exposes `ctx` to the guest in this stage.

The target rejects source modules, additional functions, classes,
actors, generics, locals, statements, function calls, arithmetic,
suspension, runtime effects, borrowed objects, map or helper calls,
recursive code, nonliteral return expressions and all unsupported
features. Source size is capped at 4096 bytes before parsing. There is no
host main wrapper or host runtime linkage. No automatic fallback occurs.

## Current focused evidence (2026-10-17)

Private exact-blob run
[39070176041](https://github.com/ores-dummy-org-oreslang-stack/ores-dummy-org-oreslang-stack-lambdas/actions/runs/39070176041)
verified 20/20 mirrored Git blobs from source head
`9d18c9fb9b14951f40237022cd15bad79085223f`, built the compiler,
passed the hardened source/object and target-ABI tests, then loaded every
generated XDP action 0..4 into a real Linux 6.17.0-1022-azure kernel and
executed each with `BPF_PROG_RUN`. A deliberately invalid one-instruction
XDP program that exits with uninitialized R0 was rejected by the real kernel
verifier with a verifier log.

This is **focused exact-blob evidence, not full repository tree evidence**.
It does not yet prove source-tree-wide CTest, multiple kernel versions,
packet/context reads, maps, helpers, BTF/CO-RE, live attachment, or network
namespace behavior.

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
3. Define dedicated typed packet/context access and bounds proofs (no
   integer reinterpretation of verifier pointer types).
4. Add map definitions, helper/kfunc gating, explicit source/deployment
   licensing policy, BTF and CO-RE only with versioned kernel ABI contracts.
5. Expand the negative verifier corpus beyond uninitialized-register proof,
   including bad context bounds, illegal helper capability and map lifetime.
6. Expand to socket/TC/tracing independently after program-type-specific
   admission and verifier proofs; keep `bpfel` itself a generic target.

**AF_XDP is a user-space packet I/O facility and does not itself certify
eBPF code.** BPF is a kernel execution target, not an actor scheduler:
source actors, GC, async/await and arbitrary imports remain excluded.
