# Native supervisor terminal durability — host protocol v1

Status: opt-in trusted-host component. Not a general guest supervision release gate.

## Ownership and proof separation

The terminal key binds supervisor and runtime IDs, original parent scheduler
domain/slot/generation, failed child domain/slot/generation, failure kind, code,
correlation, logical child ID and logical incarnation (13 uint64 fields).

The scheduler must authenticate the original typed PANIC callback carrier,
including the actual parent generation and terminal envelope. A queued receipt
or copied ActorRef is not callback authority. The read-only preflight is
rechecked at the one-use claim, serialized against parent STOP.

The journal progression is:
recorded -> physical_done -> claimed -> effect_proven -> acknowledged.

- A physical_done observation must call the native scheduler's exact-generation
  poll_done and does not prove an external effect.
- A claimed receipt is a durable one-use intent. It cannot be automatically
  replayed after a process crash or a lost host acknowledgment.
- effect_proven requires independent, authoritative host commit evidence
  bound to the terminal key and journal nonce. A SHA-256 digest provided by
  the caller alone is not proof.
- acknowledged requires a prior effect_proven entry; no stage can be skipped.
- The journal's nonce differs from the in-memory scheduler claim nonce.
  Neither proves an external effect without its own verified evidence.

## Crash/reopen decisions

| Journal phase | Permitted recovery |
| --- | --- |
| recorded | Verify physical DONE before advancing |
| physical_done | Re-establish supervisor authority before first claim |
| claimed | Quarantine: determine externally whether effect committed |
| effect_proven | Persist ACK without rerunning host policy |
| acknowledged | Reject any new claim or ACK |
| Torn, invalid, checksum-mismatched replay | Fail closed; do not truncate |

The host journal writes canonical 176-byte little-endian frames, uses exclusive
nonblocking flock, O_NOFOLLOW, O_CLOEXEC and private 0600 file permissions,
fsyncs the parent directory at creation and fsyncs each appended transition.
The journal also compares the configured pathname's device/inode with
its open descriptor at startup and on both sides of each durable append,
and rejects multiply-linked files. Any detected rebinding quarantines that
writer, including after the original pathname is restored. These checks are
fail-closed detection, **not an atomic defense against a hostile directory
owner who can rename and restore the path between checks**. The parent
directory and filesystem must remain trusted and access-controlled.
Its FNV-1a checksum is for accidental corruption detection, not adversarial
tamper protection.

## Host-bound acknowledgment reconciliation

With a durable journal installed, the old scheduler-only
`reconcile_claimed_terminal_ack` is rejected: it cannot promote an external
effect without the journal's proof/ACK sequence.

The live bridge offers `reconcile_durable_terminal_effect(proof, verifier)`
for a trusted host that still owns its native scheduler. It verifies the
original supervisor and parent generation, exact failed child generation and
terminal failure, journal nonce, and external commit digest. An authoritative
verifier runs without journal and scheduler locks. Only verified evidence may
persist `effect_proven` and `acknowledged`. If the scheduler's CONTROL enqueue
ACK was lost, the method acknowledges the original one-use scheduler claim
**after** the durable ACK. A duplicate completed ACK returns false; no
reconciliation call dispatches the supervisor policy again.

The lower-level `NativeTerminalReceiptJournal::prove_external_effect`
and `NativeTerminalReceiptJournal::acknowledge` remain trusted-host storage
operations; they **do not** finalize the native scheduler receipt. A host that
has lost its process-local scheduler after a crash must reconstruct authority
externally and may acknowledge an already fsynced `effect_proven` record
directly through the journal. It must never infer an external effect from
`CLAIMED` alone. FNV checksums are not effect signatures.

**2026-10-12 audit:** The legacy scheduler-only
`reconcile_claimed_terminal_ack` must reject whenever a durable journal is
installed, even when the native scheduler has a live `CLAIMED` receipt.
The regression fixture specifically arranges a live scheduler claim and a
`CLAIMED` journal record, supplies a verifier that returns true, and checks
that neither ACK advances. A normal scheduler enqueue acknowledgment remains
distinct from a host external-effect commitment. Raw C ABI scheduler receipt
APIs are trusted-host capabilities, not guest-accessible durable authority.

The exact-source integration gate runs the native bridge and journal tests
three times each under both strict GCC and Clang ASan/UBSan:
https://github.com/zed-pkg-test/java-app/actions/runs/38273714480
The mirror is test-only and its 12 source Git blob hashes were checked before
compilation. This focused gate does not certify full LLVM compiler or Java
backend parity.

## Unresolved release gates

The current bridge records the event when the actual callback or recovered
parent reconciliation runs. A crash before either host handoff can still lose
the scheduler's original in-memory terminal evidence. Durable capture at the
scheduler's *first terminal publication* is required for full crash recovery.

The runtime still needs a persistent supervisor registry, generation authority
and external effect ledger. The synchronous fsync can block the native CONTROL
carrier; any production rollout needs latency/backpressure testing. Existing
legacy host-only claim APIs remain privileged and do not acquire durable
authority by default. This does not implement source-compiled guest RESTART,
ESCALATE, Java backend parity, or a complete process-crash restoration path.

## Conformance and tests

TypeSpec: contracts/v1/supervision/terminal-receipt.tsp
JSON Schema 2020-12: contracts/v1/supervision/terminal-receipt.schema.json
Cross-field validation: tests/native_terminal_receipt_schema_test.py
Crash and storage tests: tests/native_terminal_receipt_journal_test.cpp
Real native bridge: tests/native_terminal_receipt_bridge_durable_test.cpp
TLA+ model: formal/supervision/NativeDurableTerminal.tla

The TypeSpec/JSON schema describes a versioned host wire contract, while the
internal binary frame is a separate persistence format. Bounded TLC and native
CTest are evidence, not machine-checked source-code refinement. Keep PR #196
draft until the full guest/compiler release gates pass.
