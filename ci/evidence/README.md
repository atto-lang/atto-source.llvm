# Source-owned LLVM M4 guest evidence

This draft is stacked on the exact M4 LLVM compiler-to-tenant execution
branch. It adds only evidence code and CI, without copying any new compiler
source or uploading native binaries.

The Rust emitter checks exact Git head/tree, source fixture SHA, observed
native tenant guest transcript and its output digest. It writes exactly
two bounded evidence files: guest-receipt.json and guest-stdout.bin.
GitHub Actions uploads them only to this private repository with a short
three-day retention and immutable artifact digest metadata.

NOT_CERTIFIED: no cryptographic signature is produced. Nonce is generated
by the producer itself, not independently verifier-issued. The GitHub
SHA-256 artifact digest is NOT a provenance signature. Public Sigstore
Rekor signing must not be enabled for this private repository without an
approved policy. The Rust verifier must reject unsigned artifacts as
release proof. Cross-batch replay and trusted runner identity remain
unverified. This is M4 fixture execution only, not production hostile
native sandboxing or five-backend parity.
