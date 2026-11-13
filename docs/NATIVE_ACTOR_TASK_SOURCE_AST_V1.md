# LLVM native actor suspends Task<int> — typed AST / compiler safety boundary

Status: **typed source boundary implementation; no compiled actor task/await guest execution**. This branch is stacked on native actor TASK/WAKE PR #1734, but **does not** replace its runtime with a foreign Task executor.

## Canonical source slice

```atto
define actor Worker as {
  let total: int = 0;
  pub suspends fnc on_message(int incoming): int {
    let Task<int> child = task { return incoming + 1; };
    let int answer = await child;
    return answer;
  }
}
```

`pub suspends mtd on_message(...)` is also accepted by the restricted parser. This is a *shape/type admission*, not an executable source construct. Both forms must currently fail in LLVM emission with the explicit diagnostic:

`native actor Task<int> requires compiler-owned continuation frame and actor TASK/WAKE ABI lowering`

## AST and compile-time ownership proof

- `ActorSuspendingTaskI64` records exactly one named `Task<int>`, a pure scalar producer expression, the one copied incoming `int` capture, one consuming `await` owner, and an immutable result binding.
- The only allowed captured variable is the `on_message(int incoming)` parameter; accepted expression nodes are integer literals, that variable, unary negation, and bounded `+`, `-`, `*` trees. This prevents actor field/heap handles, `self`, FFI calls, borrows, closures, other tasks, and host pointers crossing an unimplemented suspend boundary.
- Task and awaited result must not shadow the parameter or actor fields, and the awaited name must be the exact lexical Task owner. The return must be the exactly awaited result.
- `suspends` cannot be combined with `critical` or another suspension modifier. A synchronous actor still rejects `await`.
- Semantic validation checks the captured i64 expression only against the copied parameter, then validates the result against the newly introduced lexical awaited-result name. It **does not** manufacture a corresponding LLVM stack slot or permit native emission.
- LLVM codegen checks `actor.suspending_task` before defining actor functions and refuses this staged AST. Legacy `main`-owned SourceTask I64 pool is never a legal execution fallback for an actor.

## Compiler test fixtures (all canonical .atto)

- `native-actor-task-i64-typed-gate.atto`: valid restricted `suspends fnc` AST, emission denied explicitly.
- `native-actor-task-i64-mtd-gate.atto`: identical admission through `suspends mtd`, emission denied explicitly.
- `native-actor-task-field-capture-reject.atto`: actor-state capture denied.
- `native-actor-task-ffi-capture-reject.atto`: untrusted call/FFI capture denied.
- `native-actor-task-wrong-await-reject.atto`: wrong Task owner denied.
- `native-actor-task-wrong-return-reject.atto`: incorrect return binding denied.
- `native-actor-task-mixed-effect-reject.atto`: mixed `critical` / `suspends` denied.

Run through the real `oreslang-llvmc` compiler using the registered CTest cases:
```sh
ctest --test-dir build -R '^native_actor_suspends_' --output-on-failure
```

**Evidence:** tests are authored and registered, but on this branch no new-head executed compiler/guest receipt has been obtained. A static code read or CTest registration is NOT a test PASS. Source repository Actions have had zero-step runner admission failures. The private CI mirror allowlist and PR-specific carrier authorization must be independently approved before any transfer.

## Required next implementation before positive execution

1. Make the compiler produce an actor-owned, bounded frame containing only the copied scalar capture, continuation PC, exact actor incarnation and generation-fenced Task identity; memory must live in the actor's heap/arena, not the DATA or producer stack.
2. Generate separate code for the initial actor DATA admission and the Task WAKE continuation; wire them through `NativeActorTaskRegistry` on the exact physical actor scheduler. Do not use `SourceTaskRuntime`'s trusted-main Task pool.
3. Register Future readiness atomically with suspension, including already-completed Futures and the early-result escrow; commit `SUSPENDING -> SUSPENDED` **only** in the trusted after-WAKE callback after the guest frame physically exits.
4. Ensure STOP recursively cancels child scopes and waits for physical quiescence, rejects late WAKE, and destroys frame owners exactly once after the scheduler retirement receipt. Include adversarial negative/mutation tests.
5. Run actual compiler-generated `.atto` guest executables with GCC/Clang under O0/O2, ASan/UBSan, compare against the Java reference, obtain independent review and merge.

Host-level bridge tests, AST admission, or syntactic fixtures alone must never count as positive AttoLang backend parity.

## 2026-11-13 follow-up: actual checked producer LLVM IR (still no compiled await)

The compiler now provides the **explicitly experimental** CLI mode:

```sh
oreslang-llvmc --emit-actor-task-producer-ir-only tests/fixtures/native-actor-task-i64-typed-gate.atto
```

This first parses and validates the real `.atto` source, then invokes
`emit_native_actor_task_i64_producer_ir` on its `ActorSuspendingTaskI64`
node. It emits a native-callable LLVM function with the temporary probe ABI:

```c
/* status: 0=success, 1=signed i64 overflow, 2=null out */
int32_t atto_actor_task_i64_producer_probe(
    int64_t copied_incoming, int64_t* result_out);
```

The helper compiles exact source integer literals, one copied immutable input,
negation, addition, subtraction and multiplication to checked LLVM intrinsics
(`llvm.sadd/ssub/smul.with.overflow.i64`), with a zero-write failure path.
It never emits `llvm.trap` or `SourceTaskRuntime` calls and does not own or
allocate a future, Task, actor or additional thread.

The probe mode is restricted to host targets, exactly one validated actor Task
body, and no additional guest definitions except `main`. It refuses
other compiler modes/profiles that would imply a runtime/target certification.
**Normal `oreslang-llvmc fixture.atto` remains deliberately rejected** at
the actor continuation emission gate; the probe is not an alternative
guest-facing syntax or complete source compilation.

New **executable compiler-test definitions** (not yet executed on this head):

- `native_actor_task_fnc_compiled_producer_ir_o0_o2`
- `native_actor_task_mtd_compiled_producer_ir_o0_o2`

Each invokes the actual compiler on the `.atto` fixture, checks the emitted
IR for checked arithmetic and the absence of actor/pool/trap fallbacks, then
uses Clang to build and execute at both O0 and O2. The native harness checks
`41 -> 42` for `suspends fnc`, `41 -> 83` for `suspends mtd`, status 1
on `INT64_MAX` with unchanged output, and status 2 on a null result pointer.

Hybrid **compiled Atto producer on authentic native child-WAKE** tests:

- `native_actor_task_fnc_compiled_producer_actor_wake_o0_o2`
- `native_actor_task_mtd_compiled_producer_actor_wake_o0_o2`

These compile/link the real generated producer into
`native_actor_owned_task_wake_test.c`. The test's actor DATA turn copies its
actual 41 message input; the existing native task registry admits a child,
whose authenticated WAKE executes the compiled Atto arithmetic. The
host-owned parent callback still creates the child, calls `suspend`, and
receives early Future completion into the two-phase escrow. STOP and physical
quiescence tests remain host-authored.

**Crucial evidence distinction:** These hybrid tests would prove generated
Atto producer code can execute on the native actor's actual WAKE lane, but
they DO NOT prove that the Atto compiler generated task admission, lexical
Future subscriptions, `await` suspension/resumption, or actor-owned
continuation frame allocation. No emitted guest actor `await` exists yet.
Do not count either the producer-only or hybrid tests as full AttoLang guest
Task/await backend parity, even if their CI runs eventually pass.

To run the target suite once a runner executes actual jobs:

```sh
ctest --test-dir build -R '^native_actor_(suspends_|task_.*compiled_producer)' --output-on-failure
```

The source-private CI carrier review is tracked by #1750 and must be updated
for this branch's new immutable commit/tree before any cross-org transfer.

## 2026-11-13 continuation-body follow-up

The producer-only IR mode now emits **two** source-derived callable functions,
not just one:

```c
int32_t atto_actor_task_i64_producer_probe(
    int64_t copied_incoming, int64_t* result_out);
int32_t atto_actor_task_i64_resume_probe(
    int64_t awaited_result, int64_t* resumed_result_out);
```

The second body is emitted **only after the semantic checker proves** the
actor handler's post-`await` return is exactly its bound `let int answer`.
It copies the awaited `i64` result into a host-validated output pointer and
returns a failure status for null output; it never allocates, traps, or
unwinds guest exception state. Both functions are real LLVM compiler output
from the validated `.atto` AST.

The hybrid native TASK/WAKE test now follows this precise order for its
compiler-linked variant:

1. Authenticated actor DATA callback copies the original `41` input to
   bounded persistent scalar storage and starts the host-managed parent Task.
2. Parent WAKE registers a child and suspends; the guest callback returns.
3. Child WAKE calls the **generated producer body** on that copied DATA input,
   preserving the result for its owning parent; child physically settles.
4. A trusted host producer publishes the parent's Future readiness only
   **after** the child completion. Parent WAKE then calls the **generated
   post-await return body** with the child's result. No blocking or foreign
   Task execution happens on the actor carrier.
5. Existing host tests continue covering early completion escrow, STOP,
   generation-fenced late wake and physical resource retirement separately.

This is an important source-code-on-actor proof, **not source-generated
scheduling**: the C harness still creates and connects the parent/child Task
records, performs the `suspend` call, publishes readiness, and calls
`finish`. A real LLVM-compiled Atto actor program must do those actions via
generated frame continuations before full guest parity can be claimed.

The integration CMake harness compiles the generated LLVM IR **separately to
an object** at O0/O2, then links that object with the C native scheduler
fixture. This prevents C-language-only compile flags being applied to LLVM IR.
All four new executable compiler test registrations still require **actual
nonzero-step CI execution** and are NOT_EXECUTED at the time of authoring.

## 2026-11-13 compiler-generated fenced continuation-frame ABI (IR-only)

The IR-only compiler probe now emits three additional **compiler-generated**
functions from the validated restricted `suspends fnc/mtd` AST, alongside
the previously emitted pure task producer and post-await return bodies:

```c
int32_t atto_actor_task_i64_frame_init_probe(
    int64_t words[6], uint64_t actor_generation,
    uint64_t task_generation, int64_t copied_incoming);
int32_t atto_actor_task_i64_frame_step_probe(
    int64_t words[6], uint64_t actor_generation, uint64_t task_generation);
int32_t atto_actor_task_i64_frame_resume_probe(
    int64_t words[6], uint64_t actor_generation,
    uint64_t task_generation, int64_t* result_out);
```

The trusted host must supply **six zero-initialized 64-bit words** in
persistent, adequately aligned storage and retain them until physical
after-WAKE completion and scope retirement. This staging test uses
preallocated fixture memory; the production actor arena allocator is
not yet connected and the frame is **not** a safe general-purpose FFI
interface. Word layout is:

| Word | Meaning |
| --- | --- |
| 0 | Actor incarnation / scheduler generation |
| 1 | Compiler/host Task generation (not user-selected) |
| 2 | Immutable copied `on_message(int)` capture |
| 3 | Source-compiled Task producer result |
| 4 | Compiler continuation phase: 0=NEW; 1=QUEUED; 2=READY; 3=RETIRED; 4=FAILED |
| 5 | Fixed ABI version marker `0x4154544f5441534b` |

Status codes: **0** success, **1** checked signed-i64 overflow,
**2** null frame/result pointer, **3** invalid actor/task generation
or ABI marker, **4** incorrect phase, replay or reinitialization.

- `frame_init` requires zeroed storage, nonzero actor and Task generations.
  It refuses a second initialization or reinitialization of a retired frame.
- `frame_step` authenticates the exact stored generations and marker,
  requires QUEUED, calls the source-compiled checked scalar producer,
  records the result and moves to READY. Overflow is nontrapping,
  leaves the previous result unchanged and marks FAILED. Duplicate
  child execution is denied.
- `frame_resume` requires the same generations, marker and READY phase;
  invokes the source-compiled post-`await` return body and transitions
  to RETIRED only on success. Premature, stale and repeat resumptions are
  denied without writing the caller output.
- All three functions are pure generated AOT code: **no foreign Task
  executor, JIT, new thread, blocking wait, or implicit WAKE permission**.

The native C bridge fixture links these actual generated functions at
Clang O0/O2, initializes the persistent frame from the actor's DATA
message, runs `frame_step` in an authenticated child WAKE, publishes
the parent Future **after** child completion, then runs `frame_resume`
in the authenticated parent WAKE. The standalone C harness also checks
premature resume, stale actor generation, wrong Task generation,
double step/resume, null pointers, failure-state rejection and
signed-overflow nontrapping behavior.

**Security boundary:** The checks authenticate supplied provenance
against values stored by `frame_init`; they **do not** grant authority
to call the generated functions. The actor scheduler's current-WAKE
authentication, trusted NativeActorTaskRegistry, exact Task generation,
single-owner actor arena, physical after-WAKE acknowledgement and STOP
seal still must be enforced by production integration. An arbitrary
FFI caller holding the frame pointer is not authorized to run guest
code. This implementation is still IR-only and the **normal full source
compiler deliberately rejects actor `await`** until actor-owned frame
allocation, generated Task start, Future subscription, and compiler
WAKE callback wiring exist.

**Evidence as authored:** generated compiler code and C test
harnesses are committed, but full `oreslang-llvmc` and linked
native test executions on this PR head are **NOT_EXECUTED** until
separately approved private CI source transfer. No full guest `await`
backend parity is claimed.

## Sanitizer execution guard for the staged compiler test suite

The CTest cases for native O0/O2 producer-only and hybrid actor-WAKE
tests run a secondary CMake driver invoking Clang. This driver does
**not** inherit the parent build's `CMAKE_C_FLAGS` automatically.
A sanitizer-configured build previously risked executing uninstrumented
secondary binaries.

The source CMake registration now passes explicit `SANITIZE=ON` when
its C compiler flags include `-fsanitize=address,undefined`.
The secondary driver then forwards ASan/UBSan flags, disables UBSan
recovery, preserves frame pointers, and requires `__asan_init` in both
generated-producer and hybrid native executables before executing them.
Missing `nm`/instrumentation is a hard test failure, never a false
sanitizer PASS.

The same generated LLVM IR is compiled to separate O0 and O2 objects,
then linked to the native C harness with identical sanitizer options.
This covers the compiled producer and continuation-frame helper,
including the source-derived post-`await` return body; for the hybrid
test, it additionally instruments the host C task bridge and scheduler.
LLVM's front-end specific UBSan instrumentation still requires compiler
emission where applicable; the Clang flag and ASan symbol checks do not
prove comprehensive guest language UBSan coverage.

A source-free local toolchain smoke test using a synthetic C canary
confirmed Clang 17's `-fsanitize=address,undefined` binaries at O0 and
O2 both expose `__asan_init` and execute successfully. **This smoke
check is NOT an executed Atto compiler or native actor test.** The
approved exact-tree CI run is still needed before any backend claims.
