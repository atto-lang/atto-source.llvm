# Native `with` effects and `using` state capabilities

The native synchronous scalar actor profile supports the canonical JVM/GraalVM
state capability semantics. `with` declares an effect upper bound; `using`
acquires lexical authority. A broader effect declaration never upgrades a
read capability to write authority.

The follow-up [hardening audit](WITH_USING_HARDENING_AUDIT.md) documents fixes
for actor entry fencing, generated ABI collisions, target admission, malformed
callback arrows, and callback parser resource exhaustion.
The [formal verification report](WITH_USING_FORMAL_VERIFICATION.md) adds
universal SMT checks of compiled production predicates and a bounded TLA+
lifecycle model, with explicit assumptions and mutation controls.

```ores
define actor Counter as
  state { count: int = 0; }

  increment(int amount): int with state.read, state.write {
    return using state.write(critical |s| -> {
      s.count = s.count + amount;
      return s.count;
    });
  }

  read(): int with state.read {
    return using state.read(|s| -> s.count);
  }
end
```

`self.handle_state(|s| -> ...)` is the corresponding sealed read operation;
`self.handle_state(critical |s| -> ...)` acquires write authority. Explicit
callback modes use `critical read` and `critical write`, matching the JVM
parser. Hyphenated callable declaration modifiers remain a separate, unsupported
feature. The old signature spelling `using state.read` is rejected; write
`with state.read` in signatures.

## Enforced boundaries

- Every state capability acquisition requires the corresponding explicit
  `with` permission, even for a callback that returns a constant.
- `s.count = 7` needs write permission; `s.count = s.count + 1` needs read
  and write permission. An unused declared effect is allowed.
- Read callbacks cannot assign state. Write callbacks require `critical` or
  `critical write`. Direct `self.state`, bare state keys, unknown keys, loan
  escape, nested loans, unverified calls, and suspension are rejected before IR.
- Callbacks are synchronous pure scalar expressions, or a sequence of direct
  state assignments followed by a scalar return. Ordinary actor methods may
  have scalar parameters and return one terminal state callback. A method can
  also return a scalar without acquiring state. Unsupported callback shapes
  are rejected rather than treated as ordinary guest calls.
- Native `ActorOwnedScalarState` additionally checks actual reads/writes against
  verified inferred effects and the loan mode. The runtime rejects all nested
  sealed loans and rejects suspension even with a fenced continuation.
- Generated native state methods validate the current heap domain/epoch on
  entry, including callbacks that perform no state operations. Scoped callbacks
  have explicit parser/token/expression limits; see the hardening audit.
- Compiler output never eagerly reads a field under write-only authority.
  Read callbacks never write private fields back as a side effect.

Native heap access still requires a live scheduler-admitted actor turn and its
domain/epoch. No host lock is held across guest code. These synchronous scopes
cannot yield or run another guest callback, so state transitions occur within
one admitted physical turn. Reader capability does not allow multiple actor
threads. Logical serialization across an await and optimistic commit validation
are separate mechanisms; this profile admits no await in a state loan.

The existing legacy scalar-field `on_message` ABI remains a compatibility
profile, separate from actors declared with a `state` map. Canonical state
actors cannot mix legacy fields into their map or access keys outside a scope.
Native `spawn` still requires an `on_message(int): int` entry point. Other
generated state methods use the trusted actor-heap ABI; they are not a new
guest ActorRef arbitrary-method-call API.

## Verification and limits

`tests/with_using_state_cases.json` contains 55 admission cases (15 accepted,
40 rejected). Fourteen actor cases execute native code; the remaining positive
case verifies an ordinary function effect row. Its
runner compiles native positive cases, links them to the production
`ActorOwnedI64Heap` implementation and executes them. It also checks that
rejected cases emit no partial IR, write-only callbacks emit no hidden state
reads, and reads emit no state writes. A separate harness executes set,
increment, and read methods over four non-overlapping turns on new carriers.
An independent state model checks 32 generated histories of eight sequential
state assignments each, exercising reads before and after first writes.
`source_actor_e2e.py` includes canonical state updates through the real native
mailbox scheduler and checks persistent results and heap reclamation.

The optional JVM comparison compiles exact production Ast, Lexer, Token,
Parser, ActorStateContract, DeclaredEffectVerifier, and FunctionEffects blobs
from JVM commit `42dd7ceaadf43a4b467dd1d83f065dc50c84c0cd` (the canonical
with/using reconciliation branch). It compares admission results and records
the commit and SHA-256 of each source. This is a comparison with the JVM
parser/state/effect gates, not full Java TypeChecker, Graal guest execution,
or Native Image verification. That JVM feature branch is not JVM main.

```sh
python3 tests/with_using_state_parity.py \
  --compiler build/oreslang-llvmc --clang clang --root . \
  --java-repo /path/to/oreslang-source.java
```

Use `--sanitize` for native ASan/UBSan execution. The default CTest entry runs
without a sibling JVM repository dependency; CI can supply the explicit JVM
reference to run the cross-backend gate.

This establishes capability/effect parity for the supported synchronous i64
profile. It does **not** establish full actor-language parity: native bool,
string and reference/map/list state source lowering, general callback CFGs,
multiple composable loans, asynchronous methods, arbitrary ActorRef method
invocation, and whole-program Graal/LLVM execution parity remain separate
implementation work. These forms must stay rejected until their ownership,
effects, and scheduler lowering are checked.

## October 15 local verification

- 401/401 CTests passed on the implementation worktree.
- All 52 cases matched the exact JVM parser/state/effect gates above.
- The matrix, 32 state histories, cross-carrier method harness and real native
  mailbox execution passed ASan/UBSan with Apple Clang on macOS.
- The canonical `using` and `handle_state` Wasm actor fixtures passed private
  per-instance state, repeated-turn, and terminal lifecycle tests with LLVM 20
  Clang and Node.js.

The compiler was built against LLVM 20. Local CTest used Apple Clang for native
execution/sanitizers and LLVM 20 Clang for Wasm. Apple Clang lacks the Wasm
backend here; LLVM 20's macOS sanitizer runtime hung at process startup. Neither
failed toolchain attempt is counted as a pass. No GitHub Actions minutes or
cross-organization source transfers were needed for this local verification.

The original 52-case/401-test counts above describe the initial parity commit;
the subsequent hardening adds three comparison cases and two adversarial CTests.
