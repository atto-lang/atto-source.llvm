# Rust private-CI policy oracle (source-free)

This Rust 2021 crate tests Oreslang's cross-organization private-CI **admission invariants**, with no third-party dependencies, network access, GitHub credentials or Git writes.

Run:

```sh
cargo test --offline --manifest-path tools/private-ci-policy/Cargo.toml
```

`verify_source_free(before, after, pin)` rejects missing protected-main governance, status checks without pinned GitHub App IDs, changed or substituted repository identities, stale source-PR approval, unresolved P0 incidents, unapproved carriers, and changes between independent observations.

**It does NOT issue a capability, authorize a source export, retrieve GitHub data, or replace the publisher's live API attestations.** The real exporter must obtain GitHub metadata independently, use independently reviewed policy on the protected source branch, recheck before every Git-object write, and keep exports denied while the allowlist is empty or containment issues remain open.

This crate is the first step toward moving the **source-free decision logic and tests** from Python into Rust. The private-source publisher remains Python until a separately audited Rust replacement has equivalent exact-head, atomicity, and rollback proofs. Do not transfer this source repository to an alternate test organization merely to run Rust tests.
