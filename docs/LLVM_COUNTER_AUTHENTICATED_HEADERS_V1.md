# Authenticated cross-file Counter declaration headers (LLVM staging)

**Security boundary only; no executable actor parity.** This draft is
stacked on #1761 (source integrity), #1760 (namespace scalar AOT), #1756
(canonical Counter contract) and #1720 (native actor-state runtime probes).

## What changed

- `load_verified_actor_source_set` now returns an owning
  `ActorVerifiedSourceSet`, not merely an integrity receipt. The source
  bytes from the descriptor-checked, Git-object-hash-verified reads are
  retained privately, keyed by the seven unique manifest roles. A later
  compiler stage must consume these same bytes; rereading a path would
  reintroduce a TOCTOU source substitution risk.
- `--verify-actor-headers-only --expected-source-set-git-sha=<sha>
  <manifest>` authenticates the same immutable snapshot and checks the
  cross-file **declaration-header shape** of the exact canonical Counter
  candidate. A dedicated scanner skips line/block comments and quoted
  literal contents while checking the complete top-level header grammar.
- The actor nominal name, contract identity, single int state declaration,
  method receiver name and signature, private explicit `external mtd
  receive(ActorMail<int>)` signature and async main header must agree with
  the authenticated manifest. Public or ordinary receive declarations
  are not accepted as runtime entrypoints.
- No competing unverified source-loader path is enabled; ordinary
  `oreslang-llvmc <source.atto>` still denies this declared-actor form.

The headers-only receipt on the canonical seven source units is:

    ACTOR_SOURCE_HEADERS_ONLY units=7 namespace=counter actor=Counter contract=CounterContract field=total NO_BODY_TYPECHECK_NO_IR_NO_GUEST

The state initializer is admitted only as decimal zero for this restricted
Counter header stage. Other state types, alternate declaration grammars,
rich method signatures, imported namespaces and actual body semantics are
**not** claimed supported.

## Verification gate

CTest `native_actor_counter_source_set_integrity_only` now requires:

- Real production `oreslang-llvmc` integrity-only and headers-only
  invocation on the same canonical seven Git blobs.
- Five *rehash-valid* source/membership edits, each with a newly
  authenticated source hash and manifest Git hash: wrong namespace,
  wrong actor on state declaration, public receive spoofing,
  `ActorMail<String>` instead of `ActorMail<int>` and contract-method
  mismatch. These must fail specifically in the headers-only stage, not
  be disguised as checksum failures.
- Existing tampered receive, bad manifest, source symlink and wrong
  trusted Git hash negatives must continue failing.
- The separate native C++17 lexer/hash/path unit also exercises comment,
  quoted-brace and top-level extra-declaration cases.

The focused exact-head workflow compiles actual `oreslang-llvmc`, runs
the two named native CTests, and labels the result **NO GUEST**. A
zero-step GitHub Actions job is runner admission failure, *not* test evidence.

## What remains

The header scanner is **not** the production typed parser, a verifier
for method-body effects, or authority to dispatch a runtime lifecycle hook.
It does not yet validate that `critical-read` and `critical-write`
loans are nonescaping, that a callback cannot suspend or that
state mutation commits atomically. To reach Java/GraalVM parity, the
next compiler milestone is a typed cross-unit AST/type-checker using
the retained snapshot, followed by owned actor-state MIR, mailbox
DATA/CONTROL integration, native AOT guest execution and actual
`1|3|6|10` differential receipts.

Do not mark this draft merge-ready before exact-head compiler/CTest
execution and independent review. No private-source cross-org mirroring
is permitted without its own explicit security-approved source tree,
destination and trusted test plan.
