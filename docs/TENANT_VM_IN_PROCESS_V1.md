# In-process tenant VMs v1

One OS process hosts up to 1000 tenants, with one VM per tenant and that tenant's actors inside
it. On GraalVM a VM is a Graal isolate. On LLVM, Wasm and .NET it is what this document describes.

Status: working runtime with end-to-end evidence, including whole `.atto` programs that spawn, send
and stop. The language subset is still narrow (see [What `.atto` can do](#what-atto-can-do-in-a-vm)). All
evidence below is local: no GitHub Actions job has run this code. No backend is certified as a
production boundary.

## What a VM is

```
OS process
  TenantVmHost
    carrier pool: fixed OS threads shared by every VM, fair between tenants by CPU used
    VM "acme":   cage, actors, generation
    VM "globex": cage, actors, generation
    ... up to 1000
```

| Graal isolate | LLVM tenant VM | Mechanism |
|---|---|---|
| Separate heap | Cage | 4 GiB window + 8 GiB and 64 KiB of guard, reserved per VM, committed on demand |
| Heap-base-relative access | Addressing rule | Guest accesses are `cage + u32 base + u32 offset + displacement` and cannot name anything else |
| Trusted compiler | Lowering pipeline | Admission, validation and compile flags, in `tools/tenant-vm/lower_guest.py` |
| Isolate teardown | Retirement | Interrupt its carriers, unmap the cage; nothing to unwind |
| One failure domain per isolate | Same, except where a fault is shown to be confined | See [Restart](#restart) |

**An actor's memory.** Each actor's linear memory is its own region of the cage, so fields, data and
stack persist across turns and actors of one module do not collide. The Wasm instance itself
(globals, table, memory descriptor) is `instance` memory outside the cage, where guest load and
store instructions cannot name it. A generated bounds check keeps each actor inside its region.

**Faults.** A guest fault is a signal on a carrier. The handler sees the carrier is in guest mode and
jumps back to the turn boundary. The VM fails, and the thread takes the next turn from any tenant.
Outside guest mode the same fault is a host bug, and stays fatal. So every byte range the host
reads or writes on a guest's behalf must be checked first: that is how the first version could be
crashed by any tenant (see [Audit](#audit-findings)).

**Stack quota.** `actor_stack_bytes` limits how much stack each turn of a VM's actors may use, host
calls included. Every turn starts at the same depth on every carrier, so a second guard placed that far
below the start gives every carrier the same limit, and an actor overflows at the same depth wherever it
runs. The guard moves only when the next turn's quota differs from the last one's on that carrier, so
VMs that share a quota cost no system call per turn. Running past the quota is an ordinary
`StackOverflow`. With no quota (`0`, the default) a turn has the carrier's whole stack. A quota must be
at least 320 KiB, and must fit within the carrier's stack together with its guard. If guard placement cannot be completed, the turn is **rejected before guest execution** as a stack
quota failure; `stack_quota_misses` counts rejected turns, not unbounded execution. On a quota
switch, old-only pages may be restored, but pages shared by the old and new guards remain protected.
If restoring old-only pages fails, the failed turn is rejected before guest execution.
The affected carrier is retired immediately after finishing its turn accounting: it never
accepts another actor. The watchdog joins that thread, unmaps both its stack and alternate
signal stack, and starts a new carrier in the same stable pool slot. The new pthread_t is
published before its start gate opens. Guard-installation failure alone rejects the turn
without retiring the carrier.

A replacement may fail (for example, because the OS refuses a fresh stack mapping).
The watchdog retries at 10 ms and 100 ms, for at most three attempts, while reporting
`unavailable_carriers`, `carrier_replacements`, and `carrier_replacement_failures`.
A permanently unavailable slot reduces capacity explicitly; if every slot is permanently
unavailable, the host fails all live VMs, resolves pending calls, and rejects new VM opens
with `HostClosed`. It never runs guest code on an uncertain stack mapping.
The first startup on a fresh or replacement carrier must also register an alternate signal
stack before accepting turns. If `sigaltstack` fails, that carrier retires and is replaced
through the same bounded watchdog recovery path; no process-wide `abort()` is permitted.

Host calls run on the guest's stack, and a guest chooses how deep that is, so each host call checks for
256 KiB of remaining stack before leaving guest mode. A shortage counts as out of carrier stack, like any other overflow. Carrier stacks must
therefore be at least 512 KiB.

**Killing a VM.** Spinning guest code needs no compiler checks to be stopped. The host signals its
carriers; each abandons the turn. That is safe because the VM's memory is discarded, and guest code
holds no host locks. Host calls clear guest mode first, so a kill landing in one waits for it to
return. Interrupts name the turn they target, so a late one cannot hit another tenant.

**Hard rule for lowered code: no libc beyond leaf functions.** A kill landing inside `malloc` would
leave the allocator lock held for the whole process, and an `assert` would abort it. The pipeline
refuses any library importing something off a short list of lock-free leaf functions.

**Fairness.** Each ready VM holds one place however many actors it has, and places are ordered by
CPU used, so turn length buys nothing. `max_parallel_turns` caps its carriers. A VM that was idle
cannot bank credit.

**Deadlines count CPU.** A turn is killed for using its deadline in CPU time, with a wall-clock
backstop at 16 times that. With a wall-clock deadline, a neighbour crowding the CPU could get an
innocent tenant killed. CPU time is not work done: on asymmetric cores the same turn can cost 2-3
times the CPU on a slow core, so set deadlines with room for that.

**Quotas.** Cage memory, actors, instance memory, mailboxes, mail count, per-turn CPU and cumulative
turn time per VM. Queued mail bytes per VM and for the whole host, since mail is host memory whose
size a tenant chooses.

## The security boundary

The runtime does not enforce the addressing rule. `rule_breaking_read_is_contained_only_if_it_faults`
shows it: C guest code that breaks the rule reads host memory and nothing notices.

So whatever produces guest code is the boundary, exactly as Graal's compiler is for its isolates.
**Tenants must never supply native code.** The enforced path is:

```
.atto -> oreslang-llvmc -> LLVM wasm32 -> ADMIT -> wasm2c -> clang -> library + digest -> loader
```

What each step contributes:

| Step | What it refuses or ensures |
|---|---|
| Admit (own parser, fails closed) | Own memory, shared or 64-bit memory, any import but `env.memory` and three host calls with exact signatures, growable or large tables, start functions, unknown sections, oversized modules, functions and local counts |
| wasm2c | Full validation, with every feature disabled except an allowlist, read from the tool's own `--help`. Newer wabt enables memory64, multi-memory and tail calls by default |
| clang | No asserts, no stack protector calls, `-fwrapv`, `-fno-strict-aliasing`, `-fno-finite-loops`, stack clash protection where supported, and a hard limit on frame size, half the carrier's guard |
| Library check | Only allowlisted imports, no thread-locals, one known writable global |
| Limits | CPU time, memory and file size on every tool, since each parses tenant-chosen data. A 2 MiB module took `clang -O2` 4m37s and a 3.9 MiB one 8m17s before the size limits |
| Loader | Maps exactly the bytes it hashed, under a name unique for the life of the process, and confirms by `dladdr` that the module came from them |

**Two memory layers.** The default build has bounds checks: `memory.size` is the actor's region, an
out-of-range access is a Wasm trap, and actors of one tenant cannot read each other. `--mode guard`
removes them and relies on the cage alone. **It is a test tool, not a product option:** it exists so
that tests can prove the second layer holds if the first has a bug. Both run the same attack matrix.

**WildAccess.** A fault outside any cage means the boundary has failed: lowered code broke the rule.
It is counted in `HostStats::wild_accesses`, and `HostLimits::abort_on_wild_access` stops the process.
No test on the enforced path has produced one. A service should alarm on any nonzero count.

Sharing a process has costs that come with this design, and a Graal isolate shares them:

- A bug in this runtime, the pipeline, wasm2c or clang can reach every tenant in the process.
- A crash outside guest code ends every tenant.

### Side channels

Reviewed by reading the design, not measured. **Treat this as open:** no timing attack has been tried.

- Guests have no clock import, and no way to read one on the enforced path.
- With `max_parallel_turns` above 1 two actors could build a timer from a shared counter, but per-actor
  bounds checks leave them no memory to share. Mail latency remains as a coarse timer.
- Guard mode lets one tenant's actors share memory, which is one more reason bounds checks are the default.
- Bounds checks can be bypassed speculatively (Spectre v1). The cage limits an architectural access,
  not a speculative one, so memory of other tenants is in the speculative reach of `base + u32 + u32`
  only if a misprediction can steer outside the cage. Nothing here adds speculation barriers.
- Carriers move between tenants with no flush of branch predictors or caches. Tenant code that shares a
  core can observe contention. Dedicating carriers per tenant is the only real answer here, and it
  gives up density.
- Carrier stacks are not scrubbed between turns. Wasm locals start zeroed and Wasm cannot read
  uninitialized stack, so nothing in this path observes them. Native guests could.

x86 PKU could harden the cage against bugs in the compiler path. It is not implemented, and the
machines used here are arm64, except under emulation where it could not be tested meaningfully.

## What `.atto` can do in a VM

`oreslang-llvmc --target=wasm32 --guest-abi=tenant-vm` compiles a whole program for a VM. Without that
flag the browser ABI is unchanged, and `spawn`, `send` and `stop` are still refused.

| | |
|---|---|
| Works | Several `define actor` types with 1 to 16 integer fields; `on_message(int): int`; a `main` that spawns, sends and stops; checked arithmetic that traps; calls to top-level functions, including recursion and `if` |
| Refused by the compiler | `await`, tasks, `while` and locals in a handler, class-array fields, heap-owned regions |
| Language limit, on every backend | `ActorRef` lives only in `main`, so actors cannot message each other, and nothing reads a reply to a `send` |
| Message ABI | One `i64` in, one `i64` out |

**Kinds.** Each actor type is a kind, in source order, and `main` is the last one: the manifest records
them. A host runs a program by spawning the `main` kind and calling it once. A second call is a typed
guest error.

**Host imports.** `oreslang_vm_spawn`, `oreslang_vm_send` and `oreslang_vm_stop` are the only functions a
module may import, and admission checks each signature, since C linking would not notice a wrong one.
References are `i64`, and resolve only inside the caller's VM.

**A refused operation ends the VM.** Source has no way to see that a `spawn` or `send` was refused, say
because a mailbox is full. A turn cannot block, and carrying on would turn a lost message into a wrong
answer that looks fine. So a program sending faster than its workers drain needs a larger
`mailbox_capacity`. Whether it should wait instead is a question for the restart and pipeline work.

Two compiler changes, both only reachable on wasm32: a handler can call functions, where it used to fail
with an internal `unordered_map::at` error, and the flag above.

## Restart

A VM holds hundreds or thousands of actors, so one actor's fault should not end them all. A fault
restarts only its actor when the fault itself shows nothing else was touched. Everything else still
ends the VM.

| Fault, in a bounds-checked actor | Result |
|---|---|
| Wasm trap: overflow, out-of-range access, `unreachable`, bad indirect call | Restart |
| Out of carrier stack | Restart |
| Turn over its CPU deadline | Restart. Only that turn is stopped, and siblings keep running alongside it |
| Memory fault in the cage, which the bounds checks should have made impossible | VM fails |
| Fault outside any cage | VM fails, and is counted as a boundary failure |
| Kill, or cumulative CPU budget | VM fails |
| Any fault in `main`, which may already have spawned and sent | VM fails |
| Any fault in a kind without `ORES_VM_KIND_RESTARTABLE`, such as a guard build | VM fails |

**What a restart does.** The actor's region and instance are cleared and its heap is given back. Its fields
come back as of its last turn that finished. Its queued mail stays, in order, and runs next. The message
that failed is not retried, since it would most likely fail again. A caller gets `ActorRestarted` and
the reason. A `send` has no caller, so the VM counts it in `mail_lost`, and a VM that stops cleanly still
reports `actor_restarts` and `mail_lost`, so a run with failures cannot look like a clean finish.

**Stored state.** It is host memory outside the cage, so a guest cannot write to it except by finishing a
turn. A turn writes its fields to the slot not in use, then flips a version word with one store, so a kill
at any point leaves a complete commit. `atto_a_fault_in_a_commit_cannot_tear_state` faults exactly in a
commit, and a control build without the flip does tear, which shows the test can tell.

Two limits on what this means today:

- State is fields only. It survives a restart, not the VM or the process. That is the part of "somewhere
  like an ETS table" this needs now, and nothing here is durable across a crash of the process.
- A kill after the commit but before the reply reports a failed turn whose effect did stay. A caller who
  retries could apply it twice.

**A crash loop ends the actor, not the VM.** An actor may have `restart_budget` restarts in
`restart_window_ms`, by default 3 in 10 seconds. Stored state can be what causes the crash, so reloading
it could otherwise loop forever. A turn that works does not refill the budget; only time does. Between
tries the actor waits `restart_backoff_ms`, by default 10 ms, doubling with each failure in the window,
and its mail waits with it. These numbers are starting values, not measurements.

A restartable turn cannot send, spawn or stop, so that a failed turn has nothing outside its actor to undo. One more failure ends that actor. The failed call and each queued call get `ActorTerminated`, and queued
sends count in `mail_abandoned`. Its memory and quota go back to the VM, and later calls get
`ActorTerminated`. The rest of the VM goes on. `actors_terminated` and `mail_abandoned` are reported even
when the VM stops cleanly.

**Not built yet:**

- No pipeline semantics exist here, because source has no way to read a reply and handlers cannot send.
  The agreed rules are in `ACTOR_RUNTIME_RESILIENCE_V1`: a pipeline stops once a stage ends, and the rest
  of the tenant keeps running.
- A turn's sends must be held until it commits, so that a restart cannot resend them. This does not arise
  yet: restartable kinds are source actors, and their handlers cannot send. It must land with handler sends.
- A failed actor reports through the VM's counters and the failed call. There is no supervisor to tell yet.

## Backends

| Backend | Guest code | Boundary | Status |
|---|---|---|---|
| GraalVM | Truffle | Graal isolate, one Engine per tenant | Replaces the shared pool in `TWO_DOMAIN_EXECUTION_SECURITY.md`; not changed yet |
| LLVM | Lowered Wasm | Cage + lowering pipeline | Runs here, on the shared runtime, with real `.atto` |
| WASM | Wasm under Wasmtime | Store + Wasmtime | Spike with targeted kill, in `oreslang-source.wasm` |
| .NET | Lowered Wasm, via `TenantVmCApi.h` | Cage + lowering pipeline | Spike through the digest-pinned loader, in `oreslang-source.net` |

**Why .NET does not run tenants as CIL.** CoreCLR has no in-process boundary to build on: AppDomains
are gone and an AssemblyLoadContext is not a security boundary. It cannot kill a spinning managed
thread (`Thread.Abort` is unsupported), stack overflow ends the process, and NativeAOT cannot load
code at runtime, so per-tenant code would need the JIT. Running guests on the shared native core
avoids all of that, and tenants add no CLR GC load. Compiler-checked CIL remains possible for trusted
code. This reasoning rests on documented platform behaviour, not on a CIL experiment here.

**Wasmtime.** A `Store` runs on one thread at a time, so with one Store per tenant, that tenant's actors
do not run in parallel. Epochs are per Engine, so a targeted kill is a per-Store flag read by its
epoch callback.

**Not yet reconciled.** `oreslang-source.net` #348, `oreslang-source.wasm` #276 and conformance #332
describe process workers for untrusted actors, and `oreslang-platform-conformance`
`contracts/tenant-isolation-v1.json` requires an OS process sandbox for hostile tenants. Both conflict
with one process for up to 1000 tenants.

## Audit findings

Each was reproduced against the first version before being fixed, and has a regression test.

| # | Severity | Finding | Fix |
|---|---|---|---|
| 1 | **Critical** | A guest `send` from offset 0 made the host read the unmapped first page: one tenant ended the process, and every tenant in it | Reject ranges below the first page |
| 2 | **Critical** | On Linux the loader could return the library of another tenant: `/proc/self/fd/N` repeats, and the dynamic linker matches on name | Unique name, and a `dladdr` check |
| 3 | **Critical** | A small scratch area let alignment carry the output past its end, wrapping the capacity, so a guest could make the host read unmapped memory | Capacity is zero past the end |
| 4 | High | A 64 MiB tenant could make the host hold over 1 GiB of queued mail, up to 4 GiB, and enough of them would exhaust it | Mail byte quotas per VM and per host |
| 5 | High | "Fairness" counted turns: a tenant with long turns got about 190 times the CPU of one with short turns | Scheduling by CPU used: 1.01:1 in the same test |
| 6 | High | A wall-clock deadline let a neighbour crowding the CPU get an innocent tenant killed | Deadlines count CPU, confirmed by a control run with the old behaviour |
| 7 | High | wasm2c-generated `assert` could abort the process | Compiled out, and the import check refuses `__assert*` |
| 8 | High | Newer wabt enables memory64, multi-memory and tail calls by default | All features disabled except an allowlist |
| 9 | High | Lowering had no resource limits | Size limits at admission, and limits on every tool |
| 10 | Medium | A straddling store at the top of the cage could write a few bytes past its end on hardware that splits it | Guard extended past the worst legal address. Not demonstrated, by reasoning only |
| 11 | Medium | A call-depth counter in a thread-local shared between tenants would not be reset by a kill | Disabled; the carrier guard is the limit. It was off by default here, by a macro that changed name between wabt versions |
| 12 | Medium | Stack clash: a large frame could step over the carrier guard | Frame limit, local limit, stack clash protection. Not demonstrated |
| 13 | Medium | Host calls could leak a C++ exception through guest frames | Caught at the boundary |
| 14 | Medium | A guest-reachable path took the process-wide registry lock on every send | Removed; the carrier holds its VM |
| 15 | Low | A failed host start left carriers running over a freed core | Stopped and joined first |
| 16 | Low | Another component replacing the fault handler later would make any guest fault fatal | `open` refuses, with `SignalHandlerLost` |
| 17 | Low | The fault handler touched a thread-local that could allocate in a `dlopen`ed library | `initial-exec` TLS model |
| 18 | Low | `SIGURG` was swallowed for threads that were not carriers | Chained |
| 19 | Low | Slot generations could wrap, reviving a stale reference | Slot retired instead |
| 20 | Low | C API could leak exceptions to P/Invoke, and did not check null lengths | Caught and checked |
| 21 | **Critical** | A validated Wasm module could end the process: recursing with `spawn` and `stop` at each level makes a host call the first thing to cross the end of the stack, outside guest mode, where a fault is fatal | Host calls check for 256 KiB of stack on entry, and a shortage is an ordinary stack overflow |
| 22 | High | A restartable turn could send, spawn or stop and then fail. The restart rolled back its own state, but the other actors had already seen its effects | Restartable kinds are refused `send`, `spawn` and `stop` until these are held for the turn's commit |
| 23 | Medium | `restart_window_ms = 0` let no failure count, so the crash-loop budget never ended anything | `open` refuses a zero window |
| 24 | Medium | The watchdog took each VM's lock to end a backoff, so one VM holding its lock delayed every tenant's deadline | The watchdog only tries the lock, and a busy VM waits a tick |
| 25 | Low | A large backoff overflowed when doubled, and a large budget kept that many timestamps per actor | Backoff is capped by the window, the budget by 65535, the window by a day |
| 26 | Low | Scrubbing an actor on restart was carrier time that fair scheduling never charged | Charged to the VM like a turn |

Correcting an earlier claim: "a noisy tenant cannot starve a quiet one" was true of latency in that
test, but finding 5 means CPU share was not fair until now.

## Evidence

Run on 2026-11-03. **All of it is local**, on macOS 26 arm64 (Apple clang, wabt 1.0.42, wasm-ld 23) and
Ubuntu 24.04 arm64 in a VM (clang 18, wabt 1.0.34). `.atto` went through `oreslang-llvmc` built against
LLVM 18 in that VM, whose IR the macOS run lowered. x86_64 is under QEMU emulation.

| Claim | Result | Where |
|---|---|---|
| 1000 tenants, 100,000 **compiled `.atto`** actors, one process | 300,000 correct turns, 0.4-1.4 s including startup | macOS, Linux |
| Whole `.atto` programs run in a VM | `main` starts workers of 2 types, 23 sends arrive in order (a worker traps otherwise, and a misordered program does trap), all stop and are released. 200 rounds | macOS, Linux, x86_64 under QEMU |
| 1000 tenants each run that program | To completion in one process, 45-130 ms natively | macOS, Linux, x86_64 under QEMU |
| A failed actor restarts alone | From its last finished turn, with queued mail in order and siblings unaffected: trap, stack overflow and CPU deadline, in test guests and real `.atto` | macOS, Linux, x86_64 under QEMU |
| A spinning actor does not stop its siblings | 2000 sibling turns complete alongside it in one VM | macOS, Linux, x86_64 under QEMU |
| Kills cannot tear stored state | Deterministic fault in a commit keeps fields consistent; **control** without the flip tears. Also 300 kills at random points | macOS, Linux, x86_64 under QEMU |
| Unconfined faults still fail the VM | Memory fault, unflagged kind, guard build, kill, and a crash loop past its limit | macOS, Linux, x86_64 under QEMU |
| Restart under stress | About 2400 restarts across 240 VMs ended every way, clean under ASan, UBSan and TSan | Linux |
| A refused send cannot lose mail quietly | A full mailbox ends the VM | macOS, Linux |
| Forged references | Made-up references, and a real one used in another VM, are refused and end only the caller's VM | macOS, Linux |
| `.atto` state is private and durable | 1000 turns; separate per actor, tenant and program | macOS, Linux |
| `.atto` overflow, recursion and runaway end one VM | Trap and StackOverflow; bystander unaffected across 5 rounds | macOS, Linux |
| Stack overflow inside a host call is contained | C and validated Wasm guests end one VM, with a witness tenant. Before the fix both ended the process with a bus error | macOS, Linux |
| Hostile Wasm stays in its cage | 64 attacks in each mode, including host pointer halves and maximum displacement: 12 in its own memory, 52 stopped, 0 left. Guard mode: all 52 fault inside its own cage | macOS, Linux |
| Admission refuses hostile modules | 29 hand-written cases, 0 accepted | macOS, Linux |
| Mutated modules cannot crash the pipeline | 1000 mutations: 995 refused, 5 accepted, 0 crashed | Linux |
| Mutants that pass admission stay in the cage when run | 7,699 code mutations biased to memory and call instructions, in a module that imports the host calls: 400 accepted, half in each mode, and all 400 run with hostile inputs. No process death, no fault outside the cage, witness tenant unaffected | Linux |
| Hostile host calls cannot fault the host | 640,000 random boundary-biased calls in 400 VMs, under ASan+UBSan, with a witness tenant unaffected | Linux |
| Heap free reuses memory, tolerates abuse | 100,000 alloc/free cycles on a 64 KiB heap; poisoned free lists, double and wild frees | macOS, Linux |
| Loader admits exact bytes only | Wrong digest, tampered copy, symlink, directory all refused; 200 reloads | macOS, Linux |
| Fair by CPU | 1.01:1 with turns 200 times longer, measured during contention, was about 190:1 | macOS, x86_64 under QEMU |
| Crowded host costs no innocent turn | 120 ms CPU turn took 560-1040 ms wall under a 300 ms deadline, on 7 carriers pinned to one core, 20 of 20. **Control**: with a wall-clock deadline it was killed 5 of 5 | Linux (needs CPU pinning) |
| Mail quota holds | Per VM and host; a dead VM returns all of it | macOS, Linux |
| 1000 VMs, 100,000 test actors | About 20 ms to create, 300,000 turns in 0.5-0.7 s; RSS +100 MiB on Linux, +440 MiB on macOS (larger pages) | macOS, Linux |
| Kill spinning code without cooperation | Under 1 ms | macOS, Linux |
| ASan+UBSan on host | Clean, core and lowered, 37 + 16 cases | Linux |
| TSan on host | No races in 37 cases and in real spawn, send, stop and restart programs, but it cannot cover pure-spin kills: it defers async signals to its next interceptor | Linux |
| Host built with gcc | 37 + 16 cases | Linux |
| Existing wasm32 and rejection tests | 171 of 172 pass. `native_exit_reject_unsupported_async_root` fails the same way without this change | Linux |
| Wasmtime targeted kill | 6 spinning tenants ended one at a time, none early, worst about 2 ms | macOS |
| Wasmtime: 1000 Stores | 200,000 turns at about 1.3M/s, contained faults and memory bomb | macOS |
| .NET hosts 1000 VMs through the loader | 30,000 correct turns under CoreCLR JIT and NativeAOT, while the CLR forced 706 and 981 GCs and caught 1.4M and 2.0M null faults; wrong digest refused | Linux |
| x86_64 under emulation | 37 of 37 core cases, 16 of 16 lowered, including `.atto` lowered from IR. Timings mean nothing here | QEMU TCG, Ubuntu 24.04 |
| Real x86_64 hardware, other kernels, PKU | **Not run** | |
| Wasmtime on Linux, .NET on macOS | **Not run** | |
| GitHub Actions | **Not run**: source-org jobs fail to start on billing, and `.github/workflows/tenant-vm.yml` is ready for when that is resolved. Private source was deliberately not mirrored to another org | |

### Scale

Throughput by carrier count, 200 tenants always ready, four threads submitting work, on an idle Linux
VM with 14 vCPUs (load 0.00 at the start; g++ -O2). An 18-CPU Mac with other VMs running gave the same
shape, with less stable numbers.

| Carriers | Turns of about 140 µs: turns/s | Turns of about 16 µs: turns/s | Empty turns/s |
|---|---|---|---|
| 1 | 7k | 61k | 770k |
| 2 | 18k | 112k | 658k |
| 4 | 26k | 175k | 517k |
| 8 | 42k | 282k | 405k |
| 12 | 57k | 334k | 69k |
| 16 | 64k | 355k | 70k |
| 24 | 63k | 363k | 78k |
| 32 | 60k | 225k | 85k |

What this supports:

- Real work scales with carriers up to the CPU count: about 9 times at 16 carriers on 14 vCPUs. The
  shared ready lock is not what limits turns of this length.
- Empty turns fall off a cliff once carriers plus submitting threads exceed the CPUs, about ten times
  at 12 carriers here. A thread preempted while holding the lock stalls everyone who wants it. Real
  turns did not show this up to 24 carriers, but very short turns on an oversubscribed host would.
- **Operating rule: carriers plus the threads submitting work should not exceed the CPU count.**
- Replacing the old queue, which allocated under its lock and made a wakeup call per turn, took 8
  carriers from 396k to 559k empty turns/s on the Mac.

Sharding the ready queue would help very short turns, but it would give up exact global fairness, and
this evidence does not justify that yet.

## Running it

```sh
cmake -S tests/tenant-vm -B build/tv -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DTENANT_VM_ATTO_COMPILER=/path/to/oreslang-llvmc
cmake --build build/tv -j8
ctest --test-dir build/tv --output-on-failure
# Needs WABT for the lowered tests. Sanitizers: -DTENANT_VM_SANITIZE=ON or -DTENANT_VM_TSAN=ON
# More fuzz: TENANT_VM_FUZZ_ROUNDS and TENANT_VM_FUZZ_SEED, admission_test.py --run-mutants
```

If the compiler cannot be built here, `-DTENANT_VM_ATTO_IR_DIR=` takes IR it emitted elsewhere, and
`-DTENANT_VM_WASM_CLANG=` a clang that can target wasm32 (Apple's cannot).

## Open gates

In priority order. Done since the first version: `.atto` to a VM, code loading, host-call coverage, the
targeted Wasmtime kill, actor restart, and the audit, which found what most needed doing first.

1. **Actor-to-actor messaging, loops and locals in handlers, richer messages.** Language and compiler work,
   on every backend. The host calls this needs already exist. Handler sends must commit with the turn, and
   pipeline failure can then be built. See [Restart](#restart).
2. **Reconcile with the process-worker direction and `tenant-isolation-v1.json`.** A decision, not code.
3. **Durable heap for real programs.** `alloc` and `free` exist with size classes, and memory persists.
   Nothing compiled calls them yet, nothing suspends across `await`, and zero-GC for real programs
   depends on borrow-checker lowering to them.
4. **Run it in CI and on real x86_64.** See Evidence.
5. **GraalVM.** One Engine per tenant is not started.
6. **Wire the Wasmtime and .NET spikes into their hosts.** They are spikes in their own repositories.
7. **Very short turns on oversubscribed hosts.** Decide whether the cliff above matters before sharding the queue.
8. **Hardening.** Sandbox the lowering tools. Timing-attack testing. PKU on x86. Multi-kind modules.
