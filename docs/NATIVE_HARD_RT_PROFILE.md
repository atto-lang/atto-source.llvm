# Native hard-real-time compiler profile

Status: source-admission implementation. This is not final artifact certification.

The LLVM backend exposes the first strict source gate with:

```sh
oreslang-llvmc --rt-check program.ores
oreslang-llvmc --profile=native-hard-rt program.ores > program.ll
oreslang-llvmc --profile=native-hard-rt \
  --rt-entry=control_step/1 \
  --rt-report=rt-admission.json \
  program.ores > program.ll
```

## What the compiler proves at this stage

The hard-RT analyzer operates on the native compiler's actual parsed AST. Starting
from the selected entry, it follows the statically resolved call graph and closes
the inferred effects transitively.

It currently fails closed on:

- recursion;
- unresolved/external calls;
- generic/dynamic call qualification not yet proven;
- native Array/heap allocation outside the strict RT arena;
- blocking, suspension or unqualified I/O;
- unqualified FFI;
- throw/raise/panic paths whose cleanup bound is not proven;
- defer cleanup whose bound is not proven;
- source actors whose mailbox/scheduler bounds are not yet represented in the AST.

Literal native scalar loops may be admitted because their iteration count is
finite in the current grammar. The compiler also caps hard-RT qualification at
64 transitive call frames and 4096 functions so a finite but impractically deep
graph cannot be mistaken for qualified WCET evidence. This does **not**
establish a time-based WCET.

`using ...` is treated as a capability boundary. A function on the reachable
hard-RT path is rejected if its declared capabilities permit an effect that the
strict profile currently forbids, even when the current body does not exercise
that capability.

## Compiler report is intentionally non-certifying

The report schema is
`contracts/v1/native-hard-rt-admission-report.schema.json`.

Every report says:

- `artifact_certified: false`;
- `certification_status: "compiler-admission-only"`;
- `full_guest_aot_proven: false`;
- `linked_artifact_audit_required: true`.

The final linked executable must separately prove the complete guest AOT closure,
runtime/FFI symbol allowlist, scheduler and OS/RTOS assumptions, prefault/locking
policy, WCET, blocking, interference, jitter and deadline response bound.

## Strict RealtimeActorArena

`RealtimeActorArena` is separate from the general-purpose `ActorArena`.

The scheduler supplies:

- the control object;
- a `max_align_t`-aligned backing region;
- a unique nonzero domain/owner/incarnation identity;
- fixed slot count, slot size and scratch capacity.

Initialization zeroes and lays out the backing region before the RT window.
`ores_rt_actor_arena_seal()` performs an explicit page-touch pass and freezes
configuration before entry.

After seal, the arena has:

- no malloc/calloc/realloc/free path;
- no lock or atomic synchronization path;
- no libc memcpy/memset dependency;
- O(1) persistent slot admission through a fixed free stack;
- a max_align_t-aligned physical slot stride even when logical slot sizes are odd;
- generation-checked persistent handles;
- bounded scratch bump allocation reset at turn leave;
- bounded slot/scratch zeroization;
- caller-owned backing that is never freed by the arena.

Host buffers passed to arena read/write APIs are a trusted-runtime boundary and
must not overlap the arena backing region. The current internal byte copy has
memcpy-style rather than memmove-style alias semantics; compiler/runtime
integration must preserve this non-overlap precondition until the implementation
can enforce or support overlap directly.

The implementation deliberately uses a thread-local carrier binding so a token
cannot be replayed from another carrier. On ELF/GCC this can leave the special
`_GLOBAL_OFFSET_TABLE_` relocation marker in the object; it is not a callable
runtime import. Independent validation rejects every other undefined symbol.

## Still required

This profile is not yet the complete hard-RT runtime. The next gates are:

1. lower source actor/task/mailbox storage onto fixed RT pools;
2. connect scheduler admission and priority policy to the report;
3. add target/platform memory locking and CPU/IRQ qualification;
4. produce a linked-image allowlist audit;
5. attach WCET and response-time evidence;
6. make the final build manifest the authority for any hard-RT certification claim.

## Dedicated async and generic source fields (October 2026 gate)

The native AST stores `source_tasks`, `future_pipelines`, `async_source_i64`,
and actor lifecycle readiness/done waits outside ordinary scalar expression
and statement fields. Strict source admission must not conclude that a function
is pure merely because its `result` expression is a scalar literal. The
transitive analyzer now treats each of these as a `suspend` effect and rejects
them with `RT002_UNBOUNDED_BLOCKING`, including when reachable through an
otherwise pure helper chain. Unreachable helpers remain outside the selected
entry closure. A generic source declaration with no proven concrete
specialization fails closed with `RT005_DYNAMIC_CODE`.

This patch is a source-level safety correction, **not** support for hard-RT
async tasks and not a native artifact timing proof. Future qualified async
execution needs verified bounded scheduler, continuation-frame, cancellation,
allocation, synchronization and target-specific timing summaries before the
strict profile can admit it. The regression is
`native_realtime_transitive_effect_analysis`, backed by actual native AST
analysis in `tests/realtime_analysis_test.cpp`.
