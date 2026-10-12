# Actor sealed state and read/write critical admission v1

Status (2026-10-12): **contract plus incremental lexer/negative-check and native
logical ticket substrate; not Java/LLVM source-codegen parity**.

## Source contract

Actor `state { ... }` is a **single compiler-owned declaration**, not an
ordinary field. Never materialize it as a guest-visible `self.state`, reflective
field, dynamic property, native pointer, inherited field, or mutable alias.
Only the sealed, non-overridable `self.handle_state` intrinsic may mint an
ephemeral state capability. A class/actor cannot supply its own implementation
of `handle_state`. Type-check/validate all field accesses, including through
aliases, closures, getters, cast/dynamic/reflection and FFI.

Proposed canonical source (positive only after complete backend lowering):

```ores
define actor Foo as
    state { v: int = 0; }

    pub increment(): int using state.read, state.write {
        return self.handle_state(critical-write |s| -> {
            s.v = s.v + 1;
            return s.v;
        });
    }

    pub get(): int using state.read {
        return self.handle_state(critical-read |s| -> s.v);
    }

    receive(Message m): void {
        // self.state.v = 3;    // error: self.state is not in scope
        // val i = self.state.v; // error: self.state is not in scope
    }
end
```

The `handle_state` argument must be a lexically inline callback taking
**exactly one** state-access parameter. Unmarked callbacks remain forbidden:
`self.handle_state(|s| -> ...)` is not a synonym for a read, since mutation
and effect inference have to agree. Preexisting `critical |s| -> ...` remains
the exclusive mode for backward compatibility. `critical-write` is its
explicit new spelling; `critical-read` is a read-only mode. A read-only
callback must not call an unknown/mutable function, return/store/borrow its
state capability, or retain a state-backed alias through suspension. A write
callback may read and mutate; `using` must cover both actual effects.
A direct assignment without reading its prior value requires state.write only.

`critical` on file/static/instance callables retains existing exclusive
logical-gate behavior. Proposed `critical-read` on a callable uses the shared
read gate and `critical-write` uses the exclusive writer gate, scoped to the
same instance/code-unit as the existing critical member gate. The parser may
eventually accept modifiers before or after `fnc`; **do not** accept them as
no-op modifiers before compiled metadata and scheduler handling exist.

## Keyword tokenization

The hyphen is ASCII U+002D (`-`, HYPHEN-MINUS).
Recognize `critical-read`, `critical-write`, `transform-pipe`, `nb-select`
only as exact contiguous reserved spellings. Ordinary `a-b` and `a - b`
remain subtraction; `a->b` remains the arrow. No generalized hyphenated
identifiers, and do not combine tokens across spaces/comments. Unknown forms
fail closed; `transform-pipe` and `nb-select` are reserved without asserting
runtime syntax support.

## Actor execution, snapshots and fairness

Oreslang's **at most one physical guest executor per actor** invariant is
unchanged. Multiple logical read permits must **not** cause the same actor's
guest code to execute on multiple carriers. Shared reads can run in parallel
only on verified immutable, generation-pinned snapshots outside that actor's
single-flight guest turn; ordinary in-actor callbacks still execute serially.
Without snapshot lowering, `critical-read` must fail closed rather than
claim parallelism. A writer is exclusive against readers and writers, never
held as an OS mutex across a guest callback/await.

Admission is FIFO with consecutive readers in one cohort, bounded queue,
cancellation-aware tickets, no barging after a queued writer, and a scheduler
CONTROL wake path. Tokens are gate scoped, monotonic and generation-checked;
retirement revokes tickets. No implicit read-to-write lock upgrade (release
and retry with an application-level version check). Same-continuation nested
callbacks must fence the outer loan and use trusted reentrancy handling; a
new ticket must not deadlock its own parent. A yielding callback retires its
state loan before yielding, reacquires a fresh ticket/loan after resumption,
and does **not** promise callback-wide atomicity across await.

## Release gates

- Reject direct read, write, address-taking, aliasing, reflection, state
  shadowing or dynamic lookup of `self.state`; reject overriding handle_state.
- Reject non-inline callback, wrong arity, escaping loan, understated effect
  row, writes in read mode, unknown transitive FFI, and live loan at await.
- Verify token writer fairness, reader cohort, cancel/handoff race, stale/
  duplicate/foreign tickets, shutdown and actor exclusivity.
- Lower runtime-owned typed state for Java/Graal and LLVM before accepting
  positive source. Run identical positive/negative source fixtures on both
  backends, with sanitizer and parallel stress tests. Existing lexer/ABI
  tests alone cannot satisfy this release gate.

See `CRITICAL_HANDLE_STATE_USING_PARITY.md` and
`CRITICAL_CALLABLE_SEMANTICS.md` in the LLVM repository for prior contracts.


## Hardening audit (2026-10-12)

The first implementation found and addressed the following boundary gaps:

1. Java parsing denies direct `self.state` within actor classes and rejects
   actor-defined `handle_state` methods (including `static fnc` declarations).
   Java's type checker additionally rejects `.state` access based on a
   receiver's nominal actor type; aliases cannot bypass the literal syntax
   check. These checks do not imply the positive state schema is lowered.
2. The native RW admission substrate caps **active reader tickets at 1024**
   and queued tickets at **8192**. Writers are FIFO-ordered; arriving readers
   cannot overtake an already queued writer. Modes outside Read/Write reject
   before any ticket is minted.
3. C++ admission transfers allocated linked-list nodes from waiting to active
   ownership, without further allocation during handoff. Before changing
   ownership the release method reserves all needed wake-list space, so
   an allocation failure cannot strand a newly admitted ticket. Linear
   active-ticket lookup is the deliberate safety tradeoff pending profiling.
4. Retirement returns **all outstanding owner/waiter tokens** for terminal
   cancellation instead of silently dropping waiting continuations. Runtime
   scheduler integration MUST consume this batch and signal cancellations
   outside the gate metadata mutex. This adapter is not integrated yet.
5. Multithread cancellation/handoff, queue ordering, retirement and active
   reader bounds have targeted C++ tests. CI jobs currently fail before steps
   execute; those tests remain unverified on a build runner.

**Must remain draft:** no source-level critical-read/write effect or loan
lowering, no trusted immutable snapshot proof, no scheduler ticket binding
to guest continuations, no user-visible actor-state read concurrency, and
no demonstrated Java/LLVM end-to-end parity. A successful native gate test
is not evidence that guest actor execution is race-safe. Explicit read-to-write
upgrades are forbidden pending versioned retry semantics.
