# Independent challenge delivery — private LLVM M4

Stacked on LLVM PR #7. This adds only a self-contained Rust validator,
unit tests and a new opt-in source-owned workflow. It never copies any
compiler/runtime source or binaries to external repositories.

On push, only Rust challenge parser and negative controls run. The native
guest job is restricted to manually dispatched workflows and to protected
environment atto-private-guest-challenge-approval. Its public PEM key and
SHA-256 pin must be independently provisioned by repository administrators,
and it rejects missing configuration. A private signing key is NEVER
present on the guest runner. An independent verifier must issue the
600-second JSON challenge (via verifier PR #6) and an approved signing
authority must sign the exact raw JSON bytes with Ed25519.

The producer checks signature, the pin, repository, commit and Git tree,
fixture SHA256, workflow name, backend, feature, command, nonce and expiry,
before building and after guest execution. The only uploaded result is a
short-lived private, **unsigned**, two-file observation ZIP.

**NOT_CERTIFIED**: workflow_dispatch workflows require presence on the
default branch; this stacked draft cannot run the manual path before
authorized merge. CI push checks only contract negatives. No signing
key, GitHub environment, approved KMS, public transparency-log upload,
independent runner attestation or authenticated delivery service exists
in these changes. Long builds exceeding challenge TTL fail closed. A
signed challenge does not sign the resulting artifact and does not
prove independent guest execution or five-backend parity.

### Protected-main dispatch requirement

The manually triggered guest job additionally requires `GITHUB_REF=refs/heads/main`; **a PR-head workflow cannot run trusted guest evidence**, even if an administrator configures public-key variables. After the whole stacked source tree and workflow receive independent review and protected-main merge, a manual run can select the reviewed main revision. Before that approval, only parser/signature contract tests are available and the guest job is intentionally skipped.
