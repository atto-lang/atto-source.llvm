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
by `bpf_redirect()` or `bpf_redirect_map()`, which stage 1 does not expose.
The compiler emits the kernel entry ABI `i32(ptr ctx)`, marks section
`xdp`, and emits a `Proprietary\0` license section. This is deliberately a
**non-GPL kernel capability marker**, not a legal source-license declaration:
stage 1 has no helpers, and later GPL-only helper/kfunc support must require an
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

A separate exact-blob toolchain run
[39076709211](https://github.com/ores-dummy-org-oreslang-stack/ores-dummy-org-oreslang-stack-lambdas/actions/runs/39076709211)
built and passed the focused CTests with GCC, Clang, and Clang under
AddressSanitizer + UndefinedBehaviorSanitizer + `_GLIBCXX_ASSERTIONS`.

This is **focused exact-blob evidence, not full repository tree evidence**.
It does not yet prove source-tree-wide CTest, multiple kernel versions, maps,
helpers, BTF/CO-RE, live attachment, or network namespace behavior.

## Compiler-inferred, ungranted target closure

The experimental BPF compiler also accepts `--emit-target-closure`, but
**only** together with `--target=bpfel --bpf-program=xdp`. This emits a
version-1 resolved closure *instead of* LLVM IR, after the same parser and
BPF lowering/IR verifier have admitted the whole function:

```sh
./build/oreslang-llvmc --target=bpfel --bpf-program=xdp \
  --emit-target-closure examples/xdp_packet_u8.ores > /tmp/ores-xdp-closure.json
# Independently review and grant bpf.packet.read for this build before
# running a pinned version of oreslang-target-profiles' check-closure.
```

Constant-return XDP emits `requires: []`; checked packet-byte guards emit
`requires: ["bpf.packet.read"]`. In *both* cases the compiler emits
`grants: []`, not self-authorized authority. For the packet case an
independent build policy must explicitly grant that capability and have its
resolved graph checked. An invalid program emits neither a closure nor IR.

## Explicit packet-read acknowledgement on IR emission

The XDP compiler no longer silently issues packet-access LLVM IR: normal
IR emission of a program with any checked `packet_u8` guard requires the
explicit argument `--bpf-grants=bpf.packet.read`. Constant-action
programs require no option, and reject this unused grant. Unknown,
duplicate or compound grants are rejected. The option is invalid for
host compilation and for `--emit-target-closure` mode.

```sh
./build/oreslang-llvmc --target=bpfel --bpf-program=xdp \
  --bpf-grants=bpf.packet.read examples/xdp_packet_u8.ores > /tmp/packet.ll
```

**Security boundary:** This flag is a compile-time acknowledgement of a
capability; it is not an authorization. A caller can set the flag, so
a trusted build controller must separately check the compiler-produced
closure against policy-pinned grant declarations and bind the approved
source tree, IR and kernel object in a protected attestation. The
compiler still emits `grants:[]` in closure mode, and kernel verifier
admission + program attach permission remain separate release gates.

The manifest is **not** an attestation, signed permission, kernel-verifier
approval, compatible program-type/attach-ABI proof, or a certification that
the emitted BPF object belongs to the same source; it is only the compiler's
inferred effect declaration for the present narrow XDP slice. Kernel
verifier, ELF and attachment evidence remain independent mandatory gates.
Do not extend this shortcut to socket/TC, maps, helpers, imported functions,
or broader XDP syntax without typed effect inference and cross-repo
conformance tests. The closure schema's generic `ebpf` target deliberately
does not establish XDP attachment permission.

## Additional local effect-closure hardening (2026-10-19)

The XDP compiler now serializes both inferred target-closure variants through
the LLVM-independent `BpfEffectAdmission.hpp` helper. The emitted graph is
still version 1 and **never** includes a build grant. The full wire shape is
pinned by the registered C++ CTest; compiler-side tests also cover reordered
packet-byte comparisons, edge offsets (0, 14, 255, 4095), eight guarded reads,
and the forbidden ninth guard. The new compiler-side edge-case tests have
**not** yet been run under the full LLVM toolchain.

Local source-integrity-checked C++ tests passed with GCC and Clang at both
`-O0` and `-O2`, and with Clang AddressSanitizer + UndefinedBehaviorSanitizer
(8 effect-gate cases + 3 wire-contract assertions per run). A separate
C++-emitted-manifest → Node JSON interoperability check passed for both
constant and packet-read declarations. This is **not** a full repository build,
LLVM IR verifier execution, kernel verifier acceptance, or policy authorization
for that exact source head. Do not promote the staged XDP implementation until
those tests execute on an authorized runner and source/object provenance is
independently pinned.

## Local compiler-to-policy bridge (source-local, no cross-org checkout)

The source-local `tests/bpf_xdp_policy_bridge.py` runs an **end-to-end
build gate** when a trusted LLVM compiler, an independently pinned checkout
of `oreslang-target-profiles`, Clang with the BPF backend, Node 22, and
`readelf` are present. It never mirrors source or requests GitHub tokens.

Run for each **exact, externally reviewed source** after a trusted build
controller has pinned the compiler and policy CLI:

```sh
SOURCE=examples/xdp_packet_u8.ores
COMPILER=build/oreslang-llvmc
POLICY=../oreslang-target-profiles/bin/oreslang-target.mjs

python3 tests/bpf_xdp_policy_bridge.py \
  --source "$SOURCE" --compiler "$COMPILER" --policy-cli "$POLICY" \
  --expected-source-sha256 "$(sha256sum "$SOURCE" | cut -d' ' -f1)" \
  --expected-compiler-sha256 "$(sha256sum "$COMPILER" | cut -d' ' -f1)" \
  --expected-policy-cli-sha256 "$(sha256sum "$POLICY" | cut -d' ' -f1)" \
  --expected-policy-bundle-sha256 "$APPROVED_POLICY_BUNDLE_SHA256" \
  --grants=bpf.packet.read
```

For a constant-only source, omit `--grants`. SHA values in this example
are computed locally for demonstration; **a protected build controller must
supply independently reviewed expected hashes** for them to be a meaningful
provenance control. **APPROVED_POLICY_BUNDLE_SHA256 is mandatory** and
must be approved independently of the checkout being tested. The bridge
computes a versioned SHA-256 digest of the exact CLI, all four current
JavaScript policy modules, package metadata, and all 13 target profiles,
rejecting missing/extra profiles and unexpected modules or symlinks.
It verifies the complete bundle both before and after the build; a
single CLI file hash is insufficient. It still does not replace Git
provenance, immutable filesystem mounts or independently reviewed
source/compiler commits; unobserved mid-build races remain outside this
test script's security guarantee.

The bridge snapshots up to 4096 source bytes through a regular, non-symlink
descriptor and checks the expected SHA; compiles *that exact snapshot* to
an ungranted closure; refuses duplicate/unknown JSON keys, self-granted
privileges, unknown effects or targets; asks the external target-policy CLI
to authorize the exact inferred effects; and **only after policy success**
emits BPF IR and an EM_BPF ELF object. It checks sections, symbols and
relocations and reports unsigned SHA256 evidence for source, compiler,
policy CLI, full policy bundle, closure, IR and object. It does **not** attach to the kernel,
verify on a running kernel, sign the receipt, or confer deployment authority.

Compiler/readelf output is written into temporary files rather than\nfully captured into memory and rejected if either stream exceeds 256 KiB.\nThis is a post-execution memory/diagnostic bound, not a filesystem\nwrite quota.\n\nThe separate `bpf_xdp_policy_bridge_failclosed_offline` CTest exercises
the bridge's fail-closed logic with source-free mocks. A passing mock test
is never a replacement for the end-to-end test against real, pinned
executables, Linux verifier, and `BPF_PROG_TEST_RUN`.

## Evidence boundary

The focused CTests cover positive XDP actions, packet-bound admission,
negative source admission, direct-AST backend fail-closed checks, actual
Clang BPF object generation, ELF architecture, and required sections.
It requires a Clang distribution with the BPF backend and binutils readelf.
A successful IR verifier result alone is **not** sufficient evidence.

Remaining milestones, in order:
1. Run an **exact full-tree** build and complete CTest inventory; the current
   passing evidence is a focused 21-blob compiler/XDP slice.
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
