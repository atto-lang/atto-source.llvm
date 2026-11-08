# LLVM source-compiled actor frame slots — M2 parity slice

**Status: code authored; no hosted compiler/native execution receipt yet.**
This change is stacked on the logical native-carrier frame M1 PR #1537.
Unlike M1's independent C++ host model, this M2 slice connects the real
LLVM source compiler's closed synchronous actor-handler scalar places to
per-incarnation storage in the actual SourceActorBootstrap C actor runtime.

## Compiler-backed path under test

- Source: canonical tests/fixtures/atto-actor-frame-two-heaps.atto, plus
  legacy source actor regression fixtures.
- Frontend: real oreslang-llvmc parser and semantic admission.
- LLVM: each restricted on_message parameter, copied scalar actor field,
  and owned scalar local is an i64 place returned by the runtime-minted
  oreslang_source_actor_frame_i64_slot(domain, epoch, index) ABI.
  A genuine source value loads/stores there; carrier-local i64 allocas no
  longer back these scalar places. Three-word native heap temporary handles
  still live on the carrier stack and must drop before callback exit.
- Native runtime: SourceActorBootstrap allocates a distinct zero-initialized
  64 KiB scalar frame segment for each actor incarnation before publishing its
  actor ref. Runtime physical heap-turn admission provides the trusted domain
  and epoch, an atomic exclusive frame gate prevents overlapping callbacks,
  and thread-local authority only exists in that actor's admitted callback.
  Raw slot pointers are valid only within that synchronous callback.
- Exit: frame bytes are wiped on each constructor/handler return and again
  after physical actor DONE before freeing the allocation. STOP before READY
  still skips guest construction and retires frame bytes.
- Tests: exact compiled LLVM IR must call the frame-slot ABI and use four
  expected scalar places in the canonical dual-actor fixture. Native stdout,
  heap ownership and lifetime counts stay under the existing guest test
  oracle. New native retirement receipts bind actor identity, 64 KiB quota,
  high-water slots and actual slot calls; negative host tests deny supervisor,
  foreign domain, wrong epoch and out-of-range slot use. Guest source defining
  reserved actor frame/heap symbols must be rejected without LLVM IR.

## Boundary / not yet equivalent to Java actor stack isolation

This is a **single synchronous scalar handler frame**, not a complete
compiler-lowered per-actor call stack. The 64 KiB segment is zero-GC actor
native memory but not an independent guarded pthread stack. Unlowered guest
nested function calls, return continuations, destructors, exceptions, async
SourceTask frames, methods and actor migration are NOT covered. The C++ M1
host arena has logical suspension/migration semantics, but M2's C production
frame segment deliberately holds no frame state across an on_message return.

This cannot protect a process against arbitrary native recursion or
StackOverflowError equivalents, SIGSEGV/SIGBUS, JNI/FFI or malicious native
code. Full containment requires exhaustive stackless typed MIR/SSA lowering,
borrow root proof and possibly an independently killable OS worker. The
source actor group still lacks unified actor heap/frame resident quota.
Tenant sandbox isolation and Java/LLVM feature parity remain UNPROVEN.

## Evidence acceptance

Require hosted exact-head nonzero-step workflows, compiler output and
native guest logs under ordinary and sanitizer profiles, with immutable
source/fixture/compiler hashes. A job with steps=null or steps=[] is a
runner-admission failure, not a native failure or positive parity result.
Source-owned compiler and fixture may not be mirrored into an outside
organization without explicit authorization and independent security review.

## Next implementation layers

1. Replace the flat synchronous scalar segment with bounded typed frame
   descriptors (PC, lexical locals, operands, owned handles, generation),
   push/pop and suspended continuation roots. Account frame and actor heap
   allocations against a single per-actor and per-tenant budget.
2. Lower recursive fnc/method paths through a stackless dispatcher with
   proper tail-call behavior. Prohibit unchecked native recursive guest
   execution. Add deep non-tail recursion + sibling survival fixtures.
3. Bind frame stores to exact SourceTask suspension links and physical
   actor STOP/finalization grants. Prove cancellation, supervisor restart,
   hot-upgrade and no mailbox reentrancy across migration.
4. Require O0/O2 native compiler-backed .atto fixtures plus low-native-stack
   runs and ASan/UBSan, not host reference-model tests alone.
