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

## Evidence boundary

The dedicated CTest checks positive XDP actions, negative source admission,
actual Clang BPF object generation, ELF architecture, and required sections.
It requires a Clang distribution with the BPF backend and binutils readelf.
A successful IR verifier result alone is **not** sufficient evidence.

Separate, **still missing** milestones, in order:
1. Run an exact-tree build, CTest and cross-target object test on CI.
2. Use Linux CAP_BPF/CAP_NET_ADMIN (as applicable) and libbpf to load a
   test program, record verifier logs, and attach in an isolated network
   namespace; unload and clean up on every exit path.
3. Define dedicated typed packet/context access and bounds proofs (no
   integer reinterpretation of verifier pointer types).
4. Add map definitions, helper/kfunc gating, BTF and CO-RE only with
   versioned kernel ABI contracts and negative verifier fixtures.
5. Add differential XDP test-run fixtures, verifier rejection corpus,
   kernel/version matrix, and privilege-safe loader policy.
6. Expand to TC/tracing independently after program-type-specific proofs.

**AF_XDP is a user-space packet I/O facility and does not itself certify
eBPF code.** BPF is a kernel execution target, not an actor scheduler:
source actors, GC, async/await and arbitrary imports remain excluded.
