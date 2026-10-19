# Experimental Oreslang XDP scalar prototype

**Status:** Experimental Oreslang -> verified LLVM IR -> BPFEL ELF generation
(dependent on Clang BPF support). This is NOT target-kernel verifier acceptance,
a certified XDP deployment, or production packet filtering.

## Usage

`oreslang-llvmc --deployment-profile=ebpf-xdp --experimental-ebpf-xdp-ir program.ores > program.ll`

The `--experimental-ebpf-xdp-ir` flag is deliberately NOT sufficient by itself;
it must be paired with `--deployment-profile=ebpf-xdp`. The latter alone
still refuses emission. `--target` options and other experiments cannot be
combined with the prototype. Other deployment profiles remain blocked.

The entire accepted guest language for this first increment is:

```ores
pub fnc xdp_action(int packet_nonempty): int {
    if (packet_nonempty == 0) { return 1; } fi
    return 2;
}
```

The single argument is a **boolean-like i64, exactly 0 or 1**, not a
general packet length. 1 means that `data + 1 <= data_end` in the selected
XDP context; 0 means there is no safely addressable first byte.
This avoids exposing packet pointers or relying on packet_end subtraction.

The AST admission check enforces exactly one exported function with exactly
one by-value i64 parameter named `packet_nonempty`, final i64 return, at most
eight equality-guarded early returns and pure integer expression syntax
(literal, that parameter, unary negate, addition, subtraction, multiply).
It rejects declarations, locals, other calls, imports/modules, classes, actors,
heap, borrows/moves, FFI, async, exceptions, type arguments, and unknown effects.
The parser and AST gate together enforce a 16-KiB source cap, 16 expression
levels and 128 total scalar expression nodes. No heap or imported runtime
functions are present in the generated BPF LLVM IR.

The ELF section `xdp` contains an `i32` kernel entrypoint that receives an
opaque XDP context pointer, reads context data/data_end, and computes the
nonempty predicate with a packet bounds comparison. The guest policy is an
internal always-inline i64 function, and the wrapper checks its return action
is XDP_DROP(1) or XDP_PASS(2), otherwise returns XDP_ABORTED. The AST checker also statically evaluates every reachable result for both possible inputs and rejects other actions before emission.
The prototype emits **MIT** as testing-only BPF license metadata; the
compiler does NOT validate that a user owns the source-license rights.
Production support must require explicit authorized license metadata.

## Evidence

`tests/experimental_ebpf_xdp_source_object.py` requires real Clang with a
BPF target, compiles emitted IR into a file, and validates ELF64/EM_BPF and
the XDP/license sections. C++ `EbpfXdpAdmission` has positive/negative
AST tests; the CLI suite also rejects ordinary profile use or incompatible
experimental modes. See `CTest` and the dedicated CI workflow.

**Mandatory before deployment:** independent kernel verifier logs on an
explicitly pinned kernel; XDP test-run packet fixtures and malicious input;
byte-for-byte IR/object SHA evidence; BTF/CO-RE/relocations if required;
verified kernel compatibility and helper permissions; tenant isolation and
authorized loader; no claims of throughput or tail latency without target
hardware measurement. This prototype intentionally does NOT perform any
packet dereference or map operation.

Progression: basic packet-nonempty predicate -> verified ethertype parsing
with checked (data + fixed offset <= data_end) loads -> bounded maps and
stable schemas -> policy/rule updates via a trusted carrier Linux actor.
Never promote to certified `--deployment-profile=ebpf-xdp` merely because
Clang emits a BPF ELF object.

**Hardening audit (2026-10-19):** The LLVM packet comparison must use a
non-inbounds GEP until bounded by the verifier. The compiler proves allowed
DROP/PASS results for the entire 0/1 predicate domain using wrapping i64
bitvector evaluation, and the entrypoint independently clamps illegal output
to XDP_ABORTED. XDP_TX and XDP_REDIRECT remain forbidden. A 30,000-case
seeded differential oracle tests the AST proof. CI from the source private
organization and two test organizations has not shown a successful runner;
real BPF object and kernel verifier gates remain unverified.

## Exact-tree CI mirror and evidence (P0/P1)

Do not accept results from an alternate organization until the *complete* Git
tree SHA (including all blobs, executable modes, symlink targets, and nested
trees) matches a trusted read of this source branch from the GitHub connector.
The source is private: do not attempt a cross-org private checkout with the
runner's `GITHUB_TOKEN`, and never place source into public repositories.
The connector must transfer Git object contents to an authorized **private**
test repository and reconstruct the tree there. A source-only selection,
partial Git tree or test-specific copy is NOT exact-tree CI.

After checkout, before configuring/building:

```sh
bash scripts/verify-exact-source-tree.sh "$TRUSTED_SOURCE_TREE_SHA"
bash scripts/verify-ebpf-admission.sh g++
bash scripts/verify-ebpf-admission.sh clang++
ctest --test-dir build -R '^(experimental_ebpf_(xdp_|elf_parser_)|deployment_profile_)' --output-on-failure
```

Use the `CHECKED_OUT_TREE_SHA` line from Actions and compare it independently
to the trusted source tree. Keep PR draft until exact-tree CTest, real EM_BPF
ELF, and kernel verifier gates are satisfied. Four alternative private runner
probes (across multiple organizations) either failed before steps or did not
demonstrate available runner capacity as of 2026-10-19; no funded runner was
verified and no full-tree copy was authorized to run without one.

The ELF integrity parser now checks ELF64/ET_REL/EM_BPF architecture and
header versions, bounded executable XDP instructions, no BPF_CALL, no
unexpected nonempty executable sections, and exact test-only license bytes.
The standalone synthetic fixture tests 16 malformed/forged variants. This
proves a parser safety check, not actual eBPF object emission.
