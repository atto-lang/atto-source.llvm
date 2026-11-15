# AttoLang LLVM wasm32 actor → Volucro Wasmi security milestone

**Status: draft integration harness and trust-boundary specification.**
This is an executable cross-repository **source actor** test, not a statement
that the production LLVM runtime has achieved sandbox-equivalent containment
for arbitrary native code.

## Real compiler-to-runtime path

```text
real Counter .ores source (legacy-compatible AttoLang)
   LLVM --target=wasm32
      typed LLVM IR with actor lifecycle exports
   clang --target=wasm32 -nostdlib
      Wasm32 binary, import-free, ≤2 pages linker-reserved stack+memory
   Volucro vm-core (trusted authenticated DomainId; quota reservation)
      Volucro vm-exec-wasm (one bounded fresh Wasmi Store PER actor)
         INIT -> 1..32 on_message(i64)->i64 turns -> TEARDOWN
   Store dropped; tenant quota released after completion or trap
```

The compiler already emits the following Wasm32 exports from
`src/main/cpp/oreslang/backend/LlvmCodegen.cpp`:

- `oreslang_wasm_actor_init() -> ()`
- `oreslang_wasm_actor_on_message(i64) -> i64`
- `oreslang_wasm_actor_teardown() -> ()`

The Wasm32 actor code uses private Wasm globals, not the native
`ores_actor_i64_heap_*` host ABI. This is only protected from other
actors when the host instantiates the module in an **independent Wasmi Store**.
Merely assigning two actor IDs inside one shared Wasm linear memory does
not protect them from arbitrary Wasm memory loads.

## Run cross-repository exact-head smoke test

Check out `oreslang-source.llvm` PR #1592 and the companion draft branch
from `volucro/tenant-vm.rs`. Build this LLVM compiler and ensure
LLVM-compatible clang and Rust cargo are installed.

```sh
node tests/atto_volucro_wasm_actor_guest.mjs \
  --compiler /abs/path/to/compiled-llvm-frontend \
  --clang /abs/path/to/clang \
  --cargo /abs/path/to/cargo \
  --volucro-root /abs/path/to/tenant-vm.rs \
  --expected-llvm-head "$(git rev-parse HEAD)" \
  --expected-volucro-head "$(git -C /abs/path/to/tenant-vm.rs rev-parse HEAD)"
```

The script verifies both exact Git heads, compiles the **real**
`tests/fixtures/wasm32-single-source-actor.ores`, checks generated exports
and disallows host imports, then invokes `vm-core`'s executable Wasmi
consumer. Expected authorized guest outputs:

- Tenant A, actor A1, turns `[1,2,3]` → `[1,3,6]`
- Tenant B, actor B1, turns `[7,1]` → `[7,8]`
- Tenant A, fresh actor A2, turn `[5]` → `[5]` (no state leak)

Only executed success prints an `ATTO_VOLUCRO_WASM32_ACTOR_REAL_GUEST_PASS`
line and structured source + Wasm SHA-256 receipt. A checkout, generated
source contract or zero-step CI workflow is **not** runtime evidence.

## Boundary caveats

The Volucro process/VM currently runs on a host OS; its tenant DomainId is
authenticated *logical authority*, not hardware memory isolation by itself.
Isolation in this milestone is enforced by independent Wasmi guest Stores
with metered execution. Native LLVM actor runtime still requires mandatory
guest memory lowering/actor-local fault handling; the existing checked-offset
host gate is only a primitive, not a compiler-wide proof.

The current Wasmi batch API uses at most 32 synchronous turns, not a
persistent actor scheduler. It does not implement source guest `spawn`,
`send`, future suspension, host file/network access, actor groups, hot
upgrade, native stack-overflow containment or controlled JIT updates.

Volucro [tenant-spec actor ABI draft](https://github.com/volucro/tenant-spec)
defines the v0.1 wire contract. Volucro tenant-vm PR #44 must independently
verify shared-memory/memory64 denial; PR #42 is an unrelated incomplete
native-process sandbox. Security requires all applicable independently
enforced boundaries and adversarial exact-head guest/OS evidence.

**Reinstate source-owned runners** before promoting any security PR:
`steps=[]` means runner admission failure, not an executed pass.


## Real compiled Wasm actor adversarial checks (new)

The source-to-Volucro harness imports
`tests/atto_wasm32_actor_instances.mjs` to run adversarial lifecycle
checks against the **same compiler-generated wasm binary**, before
invoking the Volucro Rust Wasmi consumer. It proves, when executed,
per-instance state separation and continued sibling execution after
pre-init, double-init and post-teardown traps. It also rejects a
deliberately corrupted Wasm header (0x00→0xff). The
`WebAssembly.Instance` checks are Node/V8 engine evidence. The separate
`vm-core` invocation is needed for Volucro Wasmi evidence; both must
succeed before any combined PASS receipt is emitted.

For an already compiler-generated artifact the Node-only checker may be
invoked independently:

```sh
node tests/atto_wasm32_actor_instances.mjs /absolute/path/to/compiled-actor.wasm
```

A lower-tier C-to-Wasm32 engine canary executed locally and passed 15
checks. This does **not** substitute for the `.ores` source compiler,
AttoLang heap ownership, real Volucro Wasmi execution or native process
containment. On the LLVM source repository the latest compiler CI returned
`steps=[]`; the real source→Volucro integration test has not executed
on an approved runner. Follow [LLVM runner admission issue #159](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/issues/159)
and [Volucro runner issue #54](https://github.com/volucro/tenant-vm.rs/issues/54).
