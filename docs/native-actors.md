# LLVM Native AOT actor bootstrap (bounded profile)

AttoLang's LLVM backend can now compile a **limited, real actor program in native `.atto` source** to LLVM IR and then Linux x86-64 machine code. Actor `receive` functions and actor-owned methods are compiled native functions. The low-level bounded runtime, located in `runtime/actor_runtime.c`, uses pthreads and explicit allocation/free; no guest interpreter, JIT, WebAssembly runtime, or guest garbage collector is involved.

## A runnable two-actor example

```atto
define actor Foo1 as
    let int base = 0;
    receive(ActorMail<int> mail): void {
        self.base = self.base + mail.value;
        stdio.println(self.base);
        return;
    }
end

define actor Foo2 as
    let int base = 100;
    receive(ActorMail<int> mail): void {
        self.base = self.base + mail.value;
        stdio.println(self.base);
        return;
    }
end

pub fnc main(): int {
    val first = spawn Foo1();
    val second = spawn Foo2();
    first.send(1);
    second.send(2);
    first.send(2);
    second.send(3);
    return 0;
}
```

This is the **legacy executable `define actor` syntax**, accepted by the pinned frontend. The desired `use namespace` / `declare actor` / explicit `mtd` methods are currently syntax-only in the shared frontend and **must still reject native lowering**. The legacy direct `self.base` accesses shown here are only allowed within an actor's own callback and do **not** implement the future sealed `handle_state(critical-read / critical-write)` rule. That rule is a blocking gate before claiming the complete Atto actor model.

## Compile and verify

```sh
cargo build --offline --release --bin atto-llvm
target/release/atto-llvm tests/actors/pass/180-two-types-state.atto -o /tmp/atto-native-actors
/tmp/atto-native-actors   # exit 0; lines 1, 3, 102, 105 (two per-actor FIFO streams)
bash ci/test_native_actors.sh
```

The generated native `main` closes admissions and waits for all accepted messages to drain before the process terminates. Actor messages may interleave **across** actors; **within** an actor, FIFO and monotonic message sequence are preserved.

## Memory, scheduling, isolation

- One dedicated **256 KiB pthread stack** and one independently allocated, zero-initialized **64 KiB arena** per actor instance. Actor arenas are explicitly zeroed/freed after the thread joins, with no GC.
- State is stored using bounded 32-bit actor-arena-relative slots, currently at most 1,024 slots of 8 bytes each. The remaining arena supports a bounded offset allocator in the C ABI, but the full guest heap-object language lowering is not implemented.
- ActorRef is a 32-bit generation/slot-checked mailbox capability, **not a raw actor-state pointer**. The checker rejects direct remote field access and wrong message payload types.
- Up to **32 actor instances per process**; one OS thread per actor, not a work-stealing/reduction-budget scheduler. Each mailbox holds at most **128 scalar messages**.
- Linearizable admission under registry+mailbox locks. Invalid/closed/stale references fail. Mailbox full does not silently lose a message: in the current native guest emitter, a failed send triggers a trap. Returning a typed `Result` and async backpressure remain unimplemented.
- Main completion closes external admission, drains all previously accepted messages, joins workers and releases all actor-owned resources. Per-actor cancellation, child spawning from callbacks, restart and supervisor CONTROL notices are not yet supported.

## Executed evidence (no cross-org private checkout secret)

[Private exact-tree GHA run](https://github.com/athlet-o/athleto-e2e/actions/runs/43657189906) passed:
- Four true native `.atto` actor programs **20 executions each** (80 native executions): two actor types and instance separation, actor-local method call, FIFO sequence and bool messages.
- Three negative compile-time actor fixtures; remote state access, mismatched message types, and non-lowered `declare actor`/`mtd` must reject without an executable.
- C host-runtime stress: **four producers, eight actors, 1,600 accepted messages per run**, five runs (8,000 messages); stale generation rejection and deterministic drain.
- ASan/UBSan on the native `.atto` actor program and the C host-runtime stress harness.
- Existing 1,255 native scalar QUIC/HTTP3 protocol cases and compiler output hardening.

The executed source was Git tree `f40cb5044af7ffb55cbd05dd7024cc7963d6042d` at `atto-source.llvm@4e843d10e17f3264efcdd7dd1be6b7e9691f5a7f`; the private carrier's identical snapshot is `athlet-o/athleto-e2e@0ad2fd144e0df19e6d5c54ab711a4367dcdd8249`. All source blobs were copied through GitHub Git objects and the carrier checked out **its own tree**. No cross-organization token or secret was used. The machine-readable test receipt is `contracts/native-llvm-actor-aot-v1.json`.

## Release blockers and next implementation steps

This is not yet the complete actor runtime. Implement `declare actor` and explicit `mtd` lowering with namespace/type resolution, sealed state access using only critical callbacks, ownership/borrow checking across suspension, a bounded cooperative work-stealing scheduler, native supervision and CONTROL priority on panic, typed `send` Results, explicit cancellation/close and actor-child lifecycle.

Only after those are verified should the existing native `.atto` HTTP3 modules be wired to a real QUIC/TLS provider, QPACK decoder, actor-owned connections and client/server interoperability. Java/Graal, .NET and independent WASM backend runtime parity remain unverified.
