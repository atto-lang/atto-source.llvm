# Function effects and actor-state intrinsic — shared design draft

**Status:** Compiler contract proposal, not parser or runtime implementation. Preserve
existing `pure`, `nlex`, and `trap` function-signature syntax and semantics.
Their precise existing compiler behavior must be audited before any source grammar
change. In particular, do **not** redefine `nlex` or `trap` as an effect, infer
what their names mean, or silently change their interaction with `pure`.

## Separate dimensions

1. **Effects** describe observable operations: `state.read`,
   `state.write`, `suspend`, `io`, `alloc`, `block`, `ffi`, `panic`.
2. **Existing signature qualifiers** `pure`, `nlex`, `trap` retain their
   current parser/typechecker semantics. Only add compatibility rules after
   tests establish their meaning and precedence.
3. **Critical ownership** is logical actor-continuation admission and nested
   ownership depth; it is not itself an effect or an OS-thread lock.
4. **Real-time properties** (`bounded`, `no_alloc`, `no_block`) are
   compiler-verified contracts, not effects inferred from names. Purity does
   not imply bounded execution.

## Effect inference and contracts

Infer a function's effects from its body and transitively from callees.
An explicit effect signature, when syntax is finalized, constrains the
inferred effect set; it must not suppress an actual effect. Unknown calls and
FFI are conservatively effectful unless verified summaries are available.
A read-only actor-state callback contributes `state.read`, while mutation
contributes `state.write`; a read/write function may carry both as needed.
A callback containing `await` contributes `suspend`, and may have further
effects from its callees.

The sealed `handle_state` intrinsic is **effect-polymorphic**: its effective
effects are the callback effects union the actor-state accesses it performs.
The compiler must not automatically classify a read-only callback as a
state writer. No raw state capability escapes the callback; after a
suspension, state capabilities are revalidated and conflicting mutable loans
cannot remain live. The callback may suspend and the actor may reenter.

## Illustrative source only — syntax not approved

```oreslang
actor Counter {
    state { count: int = 0; }

    fnc read_count(): int {
        return self.handle_state(|s| s.count);
    }

    fnc increment(): int {
        return self.handle_state(|s| {
            s.count += 1;
            return s.count;
        });
    }
}
```

Expected inferred summaries:
```text
read_count  => {state.read}
increment   => {state.read, state.write}
```

Proposed effect-annotation spellings such as `with state.write`,
`effects(state.write)`, or `!{state.write}` are **not yet selected**.
Avoid introducing a second meaning for existing `with` until its grammar
and existing `pure`/`nlex`/`trap` combinations are tested.

## Required compiler validation

- Snapshot the actual parser/typechecker behavior of `pure`, `nlex`,
  `trap`, individually and in combinations, before extending signatures.
- Infer read-only vs read/write state effects, including nested
  `handle_state` and transitive calls.
- Propagate `suspend` through effect-polymorphic callbacks.
- Reject state-backed mutable borrows crossing conflicting reentrancy points.
- Prove same-continuation nested `critical` admission; distinguish another
  continuation on the same actor from the same logical owner.
- Reject strict real-time admission for unbounded loops, unbounded
  blocking/FFI, and incompatible live loans.
- Execute matching positive and negative fixtures in Java/GraalVM and LLVM.
  Neither backend should claim support based on this document alone.
