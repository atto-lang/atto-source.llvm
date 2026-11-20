# Confidential computing admission v1 — LLVM native runtime

**Status: staged fail-closed admission; no production hardware TEE.**
`VmSpec.require_confidential_compute` is an explicit trusted-host request.
`TenantVmHost::open` currently responds `ConfidentialComputeUnavailable`
*before* any VM allocation or registration. This is intentional. Existing
actor cages, `mmap`, checked offsets, guest lowering, and separate tenant
arenas are not AMD SEV-SNP, Intel TDX, Arm CCA or any other hardware TEE.

## Two independent isolation boundaries

1. **Infrastructure vs. confidential host**: a hardware TEE can protect
   the enclosing workload's memory against an untrusted hypervisor or host,
   subject to the specific technology's threat model.
2. **Tenant A vs. Tenant B inside the Atto process**: Atto must still
   enforce memory, scheduler, actor, socket/I/O, capability and lifecycle
   boundaries. VM-wide memory encryption does **not** enforce per-TenantVM
   confidentiality in one shared address space. Arbitrary native code/FFI
   bypassing checked-offset lowering remains unsafe for untrusted tenants.

## Requirements before enabling any provider

- A trusted host owns nonce/challenge generation, deployment identity,
  expected workload measurement and security-version/TCB policy. Verify
  signed hardware evidence against trusted vendor certificate roots,
  revocation, freshness, debug mode and an approved firmware baseline;
  never treat an environment flag or guest-provided claim as attestation.
- Bind the evidence to the *actual* runtime image and a fresh ephemeral
  key/challenge; release secrets only over a channel cryptographically
  bound to the attested workload. Fail closed before opening a VM.
- Bind tenant-specific keys/capabilities to host-minted tenant generation
  and revoke on restart/quiescence. Host-wide attestation is not proof of
  physical per-tenant isolation or of compiled native guest safety.
- Do not admit untrusted native code or unrestricted FFI through the
  trusted cage. Require secure lowering, enforced guest confinement,
  capability-mediated network I/O, bounded resources and independent
  teardown. Preserve hard-real-time budgets: quote verification and key
  provisioning happen off the actor's real-time hot path.
- Collect hardware-backed positive **and negative** attestation evidence
  (quote tamper, stale challenge, wrong measurement, revoked TCB, debug
  mode, key replay, tenant restart, sibling survival); record exact code,
  firmware and compiler identities. Until verified, label tests
  `HOST_ONLY` or `NOT_EXECUTED`, not `HARDWARE_ATTESTED`.

No attestation verifier, hardware driver, or secret-release service is
implemented in this change. See `docs/TENANT_VM_NAMING_V1.md` for the
separate, still-unverified shared-process native TenantVM target.

## Regression

```sh
cmake -S tests/tenant-vm -B build/tenant-vm
cmake --build build/tenant-vm --target tenant_vm_test
ctest --test-dir build/tenant-vm -R tenant_vm_test --output-on-failure
```

The named case
`confidential_compute_request_never_falls_back_to_plain_vm`
checks the new denial status, zero registered tenants or allocated heap,
and unchanged explicitly trusted admission. This is a host-unit test,
not a hardware-backed confidential-computing certification.
