# Native LLVM out-of-line receivers and guarded scalar state (P0)

This AttoLang LLVM bootstrap links the existing parser's **target actor header grammar** into the existing checked guest AST. It is not a second parser, a guest interpreter, or string rewriting. The concrete actor's lifecycle `receive` handler and the out-of-line `mtd` receiver bodies are parsed through the production parser and type checker, then emitted by the native LLVM backend.

## Executable syntax

```atto
use namespace counters;
declare actor Counter;

define actor Counter as
    let int count = 0;
    receive(ActorMail<int> mail): void {
        self.increment(mail.value);
        stdio.println(self.current());
        return;
    }
end

mtd (mut self: Counter) increment(int value): void {
    self.handle_state(critical-write |s| -> { s.count = s.count + value; });
    return;
}

mtd (self: Counter) current(): int {
    return self.handle_state(critical-read |s| -> s.count);
}

pub fnc main(): int {
    val first = spawn Counter();
    val second = spawn Counter();
    first.send(5);
    second.send(7);
    return 0;
}
```

```sh
cargo build --offline --release --bin atto-llvm
target/release/atto-llvm tests/actors/pass/188-critical-read-write.atto -o /tmp/guarded-actors
/tmp/guarded-actors
# exit 0; outputs 5 and 7 in either order
bash ci/test_native_actors.sh
```

The related `187-namespace-two-receivers.atto` fixture declares **Foo1** and **Foo2** in the same namespace with the same out-of-line `process_value` method name. The typed member tables resolve the correct receiver rather than dispatching on an untyped method-name string.

## Admitted capability and state rules

- `use namespace` is file-scoped nominal identity only; imported modules and linking external actor definitions are **not** enabled.
- `declare actor` must have a matching concrete `define actor ... receive ... end` in the same source. An unlinked forward declaration fails with `E_UNLINKED` rather than silently producing code.
- Out-of-line `mtd` methods accept `int` / `bool` params and `int` / `bool` / `void` results. ActorRef/heap/string/float payloads, exported module resolution, static methods, dynamic reflection and arbitrary callbacks are outside this native profile.
- Direct `self.field` access in target syntax is rejected (`E_SEALED`). State is owned by each actor's private 64 KiB arena, not a shared LLVM global. Target `receive` handlers can call actor-local methods but cannot access fields directly.
- `critical-read |s| -> s.field` lowers exactly one scalar field read. A callback with arithmetic, allocations or other expressions is not yet allowed.
- `critical-write |s| -> { s.field = expression; }` supports only one assignment with a statically pure arithmetic RHS (integer literals, method parameters, and bound scalar state fields). The enclosing receiver must be `mut self`.
- All these callbacks execute on the **owning actor's serialized mailbox worker**. This makes the scalar state operations exclusive. It is **not** a concurrent reader/writer lock, a general borrow checker, a safe suspension mechanism, or a license to run arbitrary effects inside guarded state.
- Compile-time failures include `E_READONLY`, `E_MUTABILITY`, `E_BORROW`, `E_ESCAPE`, `E_SEALED` and `E_UNSUPPORTED_CRITICAL`; negative fixtures check the required diagnostic, not merely a nonzero compiler exit.

## Evidence and limits

The first exact-tree private runner passed six native actor guest fixtures 20 times each (**120 native executions**), including out-of-line receiver methods and guarded private state; 10 fail-closed actor compiler fixtures; five 1,600-message concurrent ABI stress runs (**8,000 admitted messages**); actor ASan/UBSan; the previous **1,255** native scalar QUIC/HTTP3 protocol fixtures; and compiler output-safety tests.

- Initial execution: https://github.com/athlet-o/athleto-e2e/actions/runs/43666028980
- SHA-exact source tree: `e32e01b2e3276f8759776dd277b41baf24ecbb44`
- Existing bounded actor architecture and runtime limitations: [native-actors.md](native-actors.md)

No cross-organization checkout secrets or tokens were used: the matching full Git tree was copied to a private branch in `athlet-o/athleto-e2e`.

The expanded source and negative tests also passed on the private carrier: [63-file nominal-type and guarded-state hardening run](https://github.com/athlet-o/athleto-e2e/actions/runs/43666674489), exact tree `a6d2f2ab5155fac02d29fec426c2014c30ad4a98`. It executed the same **120 native actor guest runs** and **8,000-message ABI stress**, rejected **15 actor negatives** (including unsafe effects, immutable critical-write, nesting, complex unsupported read and unrelocated nominal type signatures), and preserved all 1,255 scalar protocol cases. The signed machine-readable receipt is `contracts/native-llvm-target-mtd-v1.json` (SHA-exact Git evidence, not a network interoperability claim).

## Nominal-type and sealed-public-state security audit

A subsequent source-focused audit closed three fail-closed gaps in this bounded native profile:

- The production type checker now bounds-checks the indices of both `Type::Named` and `Type::NamedRef`. Out-of-range indices from separately parsed target units produce the explicit `E_TYPE_INDEX` diagnostic rather than an unchecked array access/panic.
- The native target linker rejects `pub` actor-state fields with `E_SEALED`, even though the legacy concrete actor grammar accepted public fields. This prevents public metadata being attached to actor-private state.
- Out-of-line `mtd` bodies reparsed via the production frontend must exactly match the validated nominal header's receiver method name, parameter names/types, return type, and instance/static status. A name + arity match alone is no longer considered an ABI proof.

The audit adds four targeted negative `.atto` fixtures (`201` through `204`) and preserves the earlier source/runner receipts as historical evidence instead of rewriting them. A SHA-identical 69-file private runner compiled the actual native actor suite and returned **PASS**: 6 actor guest fixtures ×20 = 120 native ELF executions, **19** fail-closed actor compiler cases, **8,000** accepted concurrent mailbox messages, ASan/UBSan, the existing **1,255** scalar protocol checks and 12 earlier Python evidence checks.

- [Verified private runner](https://github.com/athlet-o/athleto-e2e/actions/runs/43669072054)
- Original executed Git tree: `c5e1f150fa628f3985a535efd83bcfcbffb03cb0` (69 blobs)
- Exact hardening receipt: `contracts/native-llvm-nominal-sealed-hardening-v1.json`

No cross-org checkout credentials were required; the private carrier's Git tree SHA matched the source repository before executing tests. This does **not** prove the complete borrow checker, concurrent reader locks, actor supervision or HTTP/3 networking.

**Still blocked:** Full general-purpose `critical-read` with concurrent readers, mutable borrows and lifetimes, nested callbacks, effect analysis, ownership across `await/select`, atomically admitted typed `send(): Result`, cancellations/restarts, supervisor priority CONTROL events for panic, scalable worker scheduling and real QUIC/TLS/HTTP3 networking. Do not merge this draft as complete Atto actor support.
