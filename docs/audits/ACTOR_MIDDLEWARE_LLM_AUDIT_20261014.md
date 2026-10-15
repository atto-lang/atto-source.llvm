# Actor middleware and neural model substrate — audit/hardening log

**Audit date:** 2026-10-14. **Scope:** Java/GraalVM PR #968 and LLVM PR #392, stacked on draft middleware and unary/stream architecture branches. Every claim below identifies its actual execution boundary.

## Findings and source fixes

1. **Identity laundering across a trusted middleware chain.** The host ActorMailboxMiddlewareBridge originally validated sender, recipient and sequence after individual stages, but could present a forged delegate-returned envelope to the first stage. Java now validates the delegate's first result **before** any stage and each stage before invoking its successor. Negative tests assert the first stage is not entered on forged identity.
2. **Intermediate unsafe payload substitution.** The prior bridge let one host stage pass a mutable or identity-bearing object to the next, relying on ActorRuntime to reject the final envelope. The bridge now permits only **same-type String-to-String sanitization** with no increase in String length at *every intermediate edge*. General scalar normalization stays in the compiler-owned prepareActorMail adapter and receives mandatory final ActorRuntime checks. This is trusted host code, not an API for untrusted guest/plugin callbacks.
3. **Actor payload type and budget integrity.** The ActorRuntime hook now rejects altered runtime-owned sender, recipient or sequence, null envelopes, unsendable/mutable substitute payloads, forged actor identity scalars, changed concrete payload classes, and replacement bytes beyond the already-reserved source mailbox budget. After validation, currentMail is the exact envelope passed to receive. Tests cover forgery, type replacement and replacement growth. Host code that mutates an original mutable object *in place* is outside this immutable-replacement guard; the hook is explicitly trusted and is not a general adversarial host-plugin isolation boundary.
4. **MoE routing correctness under load and adversarial scores.** Java NeuralMoERoutingPlan and native C++17 NeuralMoERoutingPlan implement deterministic, batched top-k expert assignment: finite nonnegative scores, unique expert/token identifiers, stable ties by expert number, 4096-token/256-expert/k<=8 bounds, atomic per-token top-k capacity admission, explicit REJECT vs DROP_TOKEN overflow, and scaled normalization without float-sum overflow. C++ additionally rejects invalid forged enum values. These are CPU **planning-only** routines, not native/GPU execution or differentiable expert routing.
5. **Tensor metadata quotas.** Java NeuralTensorAdmissionBudget atomically reserves descriptor counts and bounded byte estimates before allocating a host reservation; releases are once-only, replayed tickets are rejected, and concurrent admissions cannot overbook the configured local budget. This does not mint native tensor capabilities, reserve actual GPU memory or replace actor-runtime policy accounting.
6. **Training-step coordinator safety.** Java NeuralOptimizerStepLedger tracks distinct microbatch completion tickets for a single model-version/topology/step and supports one in-process commit or abort. It does not prove numerical gradient computation, mutate model weights, implement distributed checkpoint transactionality, or prevent replay after process restart.

## Observed executable validation

A standalone **host-only native** test build used exactly the same two Git blobs present on LLVM PR #392 at this audit:

- `src/main/cpp/oreslang/runtime/NeuralMoERoutingPlan.hpp` — Git blob `7a6452f58257caa884f34fbb9e24a5d2a0f55a7b`.
- `tests/neural_moe_routing_plan_test.cpp` — Git blob `82b33b52f586bfbafde7a52441a3ce3c82464195`.

Build/execute verdicts: **PASS** with GCC 14.2.0 C++17 `-O2 -DNDEBUG -Wall -Wextra -Wpedantic -Werror`; **PASS** with Clang C++17 using the same Release flags; **PASS** with Clang AddressSanitizer and UndefinedBehaviorSanitizer (`-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer`, including leak detection). The test catches invalid scores, duplicate score/expert IDs, top-k ties, capacity reject/drop, very large finite score normalization, empty input, and invalid overflow enum values.

Two strict-compile errors in the initial native test were found and corrected **before** these passes: a missing lambda capture and an unused typedef with `-Werror`.

**This is NOT an exact-root-tree LLVM compiler build or Oreslang guest test.** The checked native files were isolated by their matching Git blobs and compiled directly with C++17. Java source additions have been inspected for delimiter balance and have regression test sources, but completed Java Maven/JUnit/native-image builds are still needed.

## Unverified integration and security gates

- `@ActorMiddleware` and `@SupervisorMiddleware` guest semantics, async `next`/`next_val` continuation, typed `MiddlewareDecision<T>`, HTTP single-response ownership, and admission/supervisor proof are still **fail-closed**. The synchronous trusted host mail hook is not a substitute.
- Native guest `run`/stream `receive`, typed neural `TensorLease` ownership, CPU/GPU tensor kernels, native autodiff, optimizer checkpoints and a truly compiled 3-actor forward/backward program remain incomplete.
- MoE host planner input scores have not been authenticated as neural model outputs; caller must verify model/topology ticket and tensor/weight versions before launching expert work.
- Optimizer step ledger is ephemeral. Exactly-once **durable** updates require model/optimizer/RNG/state transaction proof.
- Cross-backend conformance `contracts/v1/actor-family-conformance.json` contains 162 named targets; new cases are `not_verified` until executable Java and LLVM guest proof exists. No list of design cases should be counted as tests passed.

## CI policy and next gates

GitHub Actions on the current source organizations has repeatedly failed with **zero executed steps**. Previous private validation-runner canaries on six candidate repositories also failed to allocate steps; do not mirror private source into a public repo. On an authorized private runner with available funding, mirror the **exact** source Git tree without cross-organization checkout credentials and verify tree SHA identity before JDK25/Maven/Native Image AOT+hybrid and LLVM CMake/CTest/sanitizer jobs.

**Recommended implementation priority:** trusted actor-turn continuation dispatch for guest middleware, then unary request/reply with owned HTTP codec, then real native CPU tensors + versioned leases + 3-actor forward/backward training. Maintain single-flight actor execution and no mutable state loans across await.
