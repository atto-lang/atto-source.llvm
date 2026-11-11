# Native sealed-state compiler integration

This candidate reconciles #1215 (`4aada3a07a5acbd23222b0d523b6b616b2a6018a`)
with the current-main execution/stack infrastructure from #1653
(`9ea7a08356a60abce48c9406b43f76a6dbe99d97`). It preserves both histories.
It is a draft, not full Java/LLVM/.NET parity or LLVM Spin execution.

## Executed source and ownership path

The real C++ compiler admits the bounded synchronous i64 isolated-actor slice:

```attolang
define isolated actor Counter as
  state { count: int = 0; }
  pub on_message(int value): int {
    return self.handle_state(critical-write |s| -> {
      s.count = s.count + value;
      return s.count;
    });
  }
end
```

This is the candidate's existing native source profile, not the Java `receive`
mail/void-callback profile. Exact shared guest-source parity remains required.
Canonical critical modifiers are contiguous; split spellings, direct state
access, read mutation, captured references, unknown fields, suspension and
unproven multi-write/postcommit arithmetic fail before IR publication.

`SealedStateOwnership.hpp` adds independent C++ validation in the ownership pass
and immediately before code generation. Previously, the ownership pass skipped
these handlers because they have no ordinary locals. Parser flags cannot alone
authorize them. Revalidation checks schema/slot/default/mode agreement,
value-only signatures, absence of hidden body/async/exceptional effects,
bounded pure scalar expressions, exact state names, and the one-write commit
boundary. Revalidating a mutated AST rejects stale authority. This is a local
checker; it does not replace the canonical Rust checker or complete its adapter.

The emitted LLVM definitions execute unchanged against production
`ActorOwnedI64Heap` and `NativeActorScheduler`. Host-issued domain/incarnation/turn
receipts fence field access. Separate actor heaps see independent traces and one
survives reclamation of the other. The heap-turn and scheduler bridges are trusted
host infrastructure, not reimplementations of the guest handler.

## Local execution evidence

- **17 CTests pass** on macOS arm64, with no skipped tests: 11 source metadata/
  admission boundaries, ownership revalidation, real sealed guest release and
  ASan/UBSan, complete source actor lifecycle, and retained guest bridge release/
  ASan/UBSan.
- The Rust sealed guest driver compiles two actual source handlers and runs
  read/write × heap/scheduler at O0/O2, 12 repetitions each: **96 native release
  executions**. O1 ASan/UBSan runs the four combinations three times: **12 native
  sanitized executions**. Receipts require actual field traffic and scheduler
  `processed=2`, `abandoned=0`. The existing no-GC IR auditor runs on both modules.
- The C++ ownership unit accepts two valid schemas and rejects **15 mutated
  ownership/effect/layout cases**. The same unit runs under ASan/UBSan.
- Strict Cargo Clippy passes for the combined Rust harness. Subprocesses are
  bounded and killed/reaped on timeout; no new Python harness is retained.
- All 23 newly inherited source fixtures migrate to `.atto`; existing references
  and exact-head provenance lists follow the rename. The old lifecycle script
  remains an existing harness, with its `.atto` write-case selector corrected.

Darwin does not support LeakSanitizer: address/undefined-behavior checks remain
enabled with `detect_leaks=0`; Linux keeps leak checking. No Darwin leak-check
claim follows. Four inherited CMake diagnostic arguments also needed their
semicolon removed from the selected substring so list expansion could not
split an argument before the compiler ran. The no-IR semantic boundary remains.

These are local compiler/native results, not hosted exact-tree CI receipts.

## Remaining gates

The source profile still uses one physical actor turn, a single synchronous
callback, i64 state and no queued reader/writer or suspend/resume lowering.
Sealed handler machine frames are not yet lowered through the retained logical
frame bridge; having both implementations in one tree is not that proof.
Compatible concurrent guest readers, concurrent-default actors, the Java receive
syntax, shared application-region borrowing, singletons and symbols remain open.

General object/reference lowering, complete checked ref32 coverage, per-context
logical stack lowering, actor-local arithmetic fault containment, cancellation,
drop/unwind proof and full Spin request/response capability execution remain
required. No OS address-space sandbox, general isolated-heap or tenant-VM
certification is implied by the bounded heap/turn results. Spin must keep its
LLVM backend unsupported until its actual shared source compiles and runs.
