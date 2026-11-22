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

## Portable preflight and scope

The staged `ConfidentialAttestationPreflight.hpp` rejects stale/replayed/
mismatched metadata and has no vendor quote verifier or key-release API.
Linux trusted hosts may mint 32-byte challenges through `getrandom`; on
other platforms the OS challenge helper fails closed. The preflight's
`UnverifiedClaimsMatch` result **does not authorize** a protected workload.

The source-free vectors are staged at
[atto-platform-conformance #543](https://github.com/atto-lang/atto-platform-conformance/pull/543);
the normative contract at
[atto-spec #193](https://github.com/atto-lang/atto-spec/pull/193); target
constraints at [atto-target-profiles #25](https://github.com/atto-lang/atto-target-profiles/pull/25).

## AMD SEV-SNP ABI and cryptographic primitive (M1 staging)

`SevSnpReportVerification.hpp` parses the 1,184-byte raw SNP report,
compares the **48-byte SHA-384** launch measurement and **64-byte
`REPORT_DATA`**, enforces signature algorithm, VMPL, debug policy,
signer kind and component-wise TCB floor, and rejects malformed report sizes.

It also verifies an ECDSA P-384 / SHA-384 signature across the signed
672-byte prefix **given an already trusted endorsement public key**.
It does **NOT** authenticate that key against AMD's ARK -> ASK/ASVK ->
VCEK/VLEK chain, inspect the certificate TCB extensions or chip ID,
or check AMD CRLs. A synthetically signed report can pass this primitive:
it is `HOST_ONLY` cryptographic test evidence, never `HARDWARE_ATTESTED`.

`compute_bound_report_data` computes a domain-separated SHA-512 transcript
of the verifier nonce (32 bytes), independently approved deployment identity
(32 bytes), big-endian SPKI DER length, and ephemeral TLS public-key DER.
Its 64-byte output must exactly match signed SNP `REPORT_DATA`.
This alone does not authenticate the channel or prove possession of the
matching TLS private key.

**Do not enable admission using either helper.** The full AMD product-root,
endorsement-chain/revocation/TCB verifier, fresh report collection, secure
key-release broker and actual SNP hardware tests remain unimplemented.

Local isolated smoke tests were run with GCC 14 + ASan/UBSan and Clang, and
the same source was built in an isolated CMake/CTest target. That does
NOT substitute for exact-PR-head GitHub Actions, actual vendor evidence or
compiled Atto guest execution.

New optional CMake test (requires OpenSSL::Crypto):
```sh
cmake -S tests/tenant-vm -B build/tenant-vm
cmake --build build/tenant-vm --target tenant_vm_sev_snp_report_verification_test
ctest --test-dir build/tenant-vm -R '^tenant_vm_sev_snp_report_verification_test
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
 --output-on-failure
```

## Offline VCEK certificate validation (M2 staging)

`SevSnpVcekEndorsement.hpp` adds a separate offline OpenSSL verifier.
It requires a trusted host-provisioned **pinned AMD ARK DER SHA-256
fingerprint**, ARK → ASK → VCEK X.509 chain, DER issuer CRLs covering
**both VCEK and ASK**, and matching Family-19h `structVersion=0`
TCB extensions (BL, TEE, SNP, microcode) and 64-byte report `CHIP_ID`.
It then verifies the report signature using the validated VCEK key.

The trust anchor PIN MUST be sourced independently from approved AMD
product certificate material. Computing the expected pin from evidence
received in the same request is a trust-on-first-use bypass. Missing,
expired or invalid CRLs fail closed. This staging verifier explicitly
does not implement cloud VLEK or Turin/family-1Ah certificate parsing.

**Status remains HOST_ONLY / NOT_EXECUTED_EXACT_HEAD**. This is not yet
an independently audited endorsement verifier, a live report collector,
a TLS key-provisioning service or real-hardware attestation. It cannot
enable `TenantVmHost::open` or release any credential. Isolated
sanitizer smoke exercised missing/forged-root refusal; the exact
repository CMake target is staged but has no qualifying executed CI.

```sh
cmake -S tests/tenant-vm -B build/tenant-vm -DTENANT_VM_SANITIZE=ON
cmake --build build/tenant-vm --target tenant_vm_sev_snp_vcek_endorsement_test
ctest --test-dir build/tenant-vm -R '^tenant_vm_sev_snp_vcek_endorsement_test
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
 --output-on-failure
```

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
