# Native TenantVM source AOT security admission (LLVM)

**Status: draft, not executed against a runner, not a native sandbox.**
This stage is stacked on PR #1890 and advances issue #1170. It does
not supersede the host object-handle PR #1892 or checked-ref32 PR #1894.

## Threat and policy

An LLVM native guest called through `OresVM::invoke_guest` can access the
same process address space as the root host. Separate actor pthread pools,
logical heap bounds, opaque `call_data` cookies, and `UNTRUSTED` scheduler
labels do not constrain arbitrary native instructions or FFI.

`oreslang-llvmc --isolate-entry=tenant_step` historically emitted a
**trusted** synchronous scalar guest. The new optional
`--tenant-execution=trusted|untrusted` contract makes the trust requirement
explicit for consumers that provide a policy. The legacy omitted option
remains trusted-only for compatibility, NOT a safe default for a tenant
that has been classified as untrusted by an external ingress broker.

- `--tenant-execution=trusted --isolate-entry=tenant_step` follows
  the existing strict pure scalar entry path; it **does not certify
  native isolation** or grant FFI.
- `--tenant-execution=untrusted --isolate-entry=tenant_step` exits with
  deterministic `TENANTVM_UNTRUSTED_SANDBOX_UNAVAILABLE` before reading
  the source or emitting any LLVM IR.
- `--tenant-execution` without an explicit isolate entry is rejected.
  Repeated or unknown execution policies are rejected. Target switching
  to wasm32 does **not** silently turn the source LLVM compiler into a
  verified Wasmtime host.
- This gate is only for the `--isolate-entry` compiler mode. The
  full native host and other actor compilation profiles are still
  trusted-only and must be denied by the external admission broker for
  hostile tenants. There is no unsafe/native trusted-to-untrusted
  promotion or ambient FFI grant.

The compiled wrapper additionally rejects a zero tenant generation
before invoking any load/store callback; this is metadata sanity
validation, not a cryptographic or memory-sandbox authority proof.
Runtime callback authority is checked by OresVM's thread-bound
single-use tickets from PR #1890.

## Executable source regression

`tests/fixtures/tenantvm-trusted-scalar.atto` is a new, actual AttoLang
fixture with a public `tenant_step(int old, int delta): int`. It
does not use a hard-coded LLVM module or a host-side emulation.

Run from a source checkout with the compiled `oreslang-llvmc`:

```sh
bash tests/tenantvm_atto_source_aot_gate.sh \
  /path/to/oreslang-llvmc clang clang++ "$PWD"
```

The script requires 7 deterministic compiler negatives with **no IR
on stdout**, including an untrusted source path that must not be read.
The positive route compiles **the real `.atto` AST** with an explicit
trusted mode into ABI-v2 LLVM IR, then Clang native objects at O0 and O2,
links `tests/native_tenantvm_atto_guest_test.cpp`, and executes both.

The native test covers two independent mutable guest state domains,
parallel same-tenant guest calls, other-tenant progress, signed overflow
without partial state mutation, foreign allocation rejection, tenant-ID
reincarnation and stale handle refusal. It also calls the generated
entry with a zero-generation/wrong-version/wrong-slot-count ABI context;
no forged host callback is permitted to run for those invalid inputs.
Its checks remain present in `-DNDEBUG` builds.

**Evidence classification:** REAL_SOURCE_COMPILED_GUEST only if this
exact branch/head's compiler, Clang, and host test binary actually
execute successfully. A workflow with `steps=[]` remains
NOT_EXECUTED; merely adding a test gate is not evidence it passed.

## Next blockers before secure untrusted execution

1. Tie allocation handles and borrow permissions to **physical actor turn**
   credentials, including generation fencing across suspension and
   retirement; avoid granting actor objects using tenant identity alone.
2. Integrate verified original-source and target-object liveness proofs
   into actual `.atto` load/store lowering. PR #1894 covers the ref32
   source arena *range*, not yet allocation provenance.
3. Enforce compiler-wide no-raw-pointer/no-native-FFI verification for
   untrusted code and audit every imported host symbol/indirect call.
4. Add authenticated process/Wasm worker creation, fuel/deadline and
   memory/resource quotas, IPC identity and fault isolation. Until then,
   **explicit untrusted compilation must keep failing closed**.
5. Treat MPK as an optional additional per-thread heap defense, never a
   replacement for verifying native guest instruction and callback authority.
6. Run exact-head compiler and native tests, sanitizer matrices, and
   negative escape/crash tests before production promotion.
