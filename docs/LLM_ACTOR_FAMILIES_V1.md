# LLM-native actor families — engineering contract (2026-10-14)

**Status: design; not implemented neural/tensor execution.** Grounded in the provided actor-native LLM architecture notes. This specification does not assert that neural actor syntax, native tensor/autodiff kernels, or GPU code generation already exist.

## Organizing principle

Use Oreslang's existing one-physical-guest-lane actor object model; do not create a separate scheduler or mutable state model for each neural family. Actor role, unary/stream reply mode, HTTP/TCP/WS/mailbox transport, SHARED/PRIVATE/UNTRUSTED isolation, and request/message admission are independent dimensions. ProxyActor remains the one canonical cross-process typed proxy family.

Actors coordinate bounded neural workloads at the scale of layers, fused stages, expert batches, generation sessions, or training microbatches. Do **not** create an actor per neuron, parameter, elementary tensor op, or token-to-expert assignment. Matmul/attention/norm/optimizer/autodiff remain native CPU/GPU kernels beneath actor scheduling.

## Proposed types and workload contracts

| Type / role | Default protocol | Ownership and task |
| --- | --- | --- |
| ModelActor<Input,Output> | unary | Parent specialization for typed model component transforms, not a new scheduler |
| LLMActor<T> | stream or unary by specialization | Model-aware generation/training and model-version constraints |
| TokenizerActor | unary | Batch strings and token IDs tied to tokenizer/version |
| EmbeddingActor | unary / pipeline | Embedding tensor leases from token batches |
| AttentionStageActor | pipeline | Fused attention with async native kernel fences |
| FeedForwardStageActor | pipeline | Fused MLP stage with shape/layout proof |
| ExpertRouterActor | unary / pipeline | Batched MoE top-k routing and capacity planning |
| ExpertActor<I,O> | unary / pipeline | Versioned expert weights, batched token work |
| ExpertCombineActor | pipeline | Weight and shape checked expert output combination |
| KVCacheActor | resource/stream | Request- and generation-scoped cache leases, quotas, eviction |
| SamplerActor | stream | Token sampling, seed, stopping rules, deterministic replay |
| GenerationActor | stream | Prefill/decode orchestration, streaming cancellation, backpressure |
| RecurrentMemoryActor | stream | Explicit request, tenant and model-version-fenced persistent memory |
| TrainingStageActor | pipeline | Forward activations, backward gradients, microbatch provenance |
| LossActor | unary / pipeline | Native loss reduction and gradient seed |
| OptimizerActor | unary / critical | Once-only optimizer update of coherent parameter version |
| CheckpointActor | resource service | Atomic model + optimizer + RNG + step snapshot |
| ModelSupervisor | supervisor control plane | Admission, health, placement, restart, topology version |
| ProxyActor<M> | unary or stream | Authenticated distributed model/expert link, no remote state access |

These are proposed *typed contract families*, not new accepted keywords such as "define llm actor". Prefer actor classes implementing capability interfaces; add dedicated source keywords only after verified compiler/runtime lowering.

## Contracts and schemas

A request-scoped neural envelope has: model ID and version, topology epoch, request ID, batch and microbatch ID, training step when applicable, sequence position, tensor dtype/shape/layout, device placement, and runtime-minted tensor lease generation. Typed resource caps cannot be forged through ActorMail fields or middleware headers.

TensorLease<T,Shape,Device> denotes an opaque native allocation descriptor with bounded byte ownership, generation/expiry and completion fence. No raw GPU pointer, mutable cross-actor tensor alias, untrusted host FFI handle, or unmetered full-tensor serialization through a mailbox. rt borrow / rt take / rt share apply; guest actor state remains in a sealed state block with using state.read / state.write(critical ...) grants. No loan crosses await/select or GPU completion without a checked release/reacquire.

An asynchronous GPU kernel submission schedules work and yields an actor-owned Future completion event; it does not allow two carriers to execute guest code in the same actor. Quotas cover mailbox items and bytes, pending kernels, leased GPU memory, token budgets, and outstanding generations. Late completions after cancellation/topology replacement are dropped.

## Graph archetypes

**Dense decoder inference:** input -> TokenizerActor -> EmbeddingActor -> fused AttentionStageActor / FeedForwardStageActor -> SamplerActor -> GenerationActor. The compiler may fuse adjacent compute stages when actor scheduling overhead is not worthwhile.

**MoE:** batched hidden-state data -> ExpertRouterActor -> bounded ExpertActor token-batch groups -> ExpertCombineActor -> next model stage. Require top-k/capacity factor, routing weights, overflow policy, byte credits, and expert load balancing; no one-mailbox-per-token anti-pattern.

**Persistent / dynamic / recurrent:** GenerationActor consults RecurrentMemoryActor and selects a bounded graph of model stages. Declare maximum traversal budget, cancellation, checkpoint, memory tenancy, and whether memory is retrieved vs trainable/differentiable. Mutable persistent memory by itself does not provide backpropagation-through-time.

**Distributed:** ProxyActor transports authenticated, versioned, typed descriptor+payload messages across IPC/TCP/WS with byte/item credits, negotiated codec and topology generation. A remote peer does not receive actor state pointers or supervisor CONTROL authority.

## Training, forward and backward

self.forward(ForwardBatch) routes **activations** downstream; self.backward(GradientBatch) routes **already computed** gradients upstream. These are directional actor messages, **not automatic differentiation**. Native autodiff supplies gradients and activation-tape/recompute rules; GPU collectives synchronize expert and model-parallel gradients.

A coherent training ticket records model_version, topology_epoch, step_id, microbatch_id and parameter_snapshot. OptimizerActor applies exactly one committed update per step following a complete accumulation barrier. A failed actor must restore a consistent model + optimizer state + RNG + scheduler position, or abandon the incomplete step. Never restart training with unrelated weights.

## Proposed executable milestone

Build one small decoder-only CPU baseline with THREE compiled Oreslang actors:
1. EmbeddingStage sends typed tensor activations through self.forward.
2. TransformerStage executes *real native tensor math*, saves/recomputes a bounded tape, forwards activations and routes computed gradients upstream with self.backward.
3. LossStage computes a real scalar loss and gradient seed; an optimizer step is committed after all microbatches finish.

Compare model outputs, loss, per-parameter gradients and one optimizer update to a non-actor baseline running the same tensor kernels. Run Java/Graal and LLVM guest fixtures, memory/ownership/cancellation faults, native sanitizers, and an exact-tree authorized PRIVATE CI build. Do not claim training parity from actor-message-only tests or a mock tensor kernel.

Next: batched MoE experiments, then bounded recurrent/persistent memory research. Throughput, tokens/sec, communication latency, GPU utilization, batch queue depth, and numerical equivalence must be measured; actors are an orchestration advantage, not automatic performance or quality improvements.

## Middleware integration

Trusted ModelSupervisor middleware owns tenant/model authorization, admission, request budgets and device placement. Actor-side @ActorMiddleware may normalize typed application metadata, validate model schemas, enrich safe traces and short-circuit unary requests with MiddlewareDecision<T>. GenerationActor is stream-mode and uses a void around outcome with bounded streaming outputs, not a fabricated unary value. No middleware may grant itself tensor authority, forge request identity, or bypass a security check using a client-provided idempotence flag.

The newly introduced Java ActorCodeExecutor.prepareActorMail hook is a *synchronous trusted in-turn pre-dispatch barrier*, not general guest async middleware. It revalidates sender/recipient/sequence/capabilities; guest annotation dispatch remains gated. Native LLVM has no executable neural actor lowering yet.

## Release gates

Compiler typing of family interfaces and envelope schema; native tensor lease ownership; 3-actor real compiled forward/backward; bounded ingress/egress, cancellation, stale GPU completion and generation fencing; optimizer consistency and checkpoint crash tests; batched expert routing; async GPU execution; cross-backend numeric parity; authorized private CI. Each gate requires observed executable tests rather than design-only declarations.

## Host-side audited planning primitives (2026-10-14 hardening)

Java now provides three **non-guest, non-GPU** building blocks for the future model runtime:

- **NeuralMoERoutingPlan**: deterministic batched top-k expert planning for up to 4096 token descriptors, 256 experts and k<=8. Each token provides one finite nonnegative score per expert; stable ties favor the smaller expert index. Its selected top-k set is admitted atomically within the configured per-expert capacity, or the entire token is dropped/rejected using an explicit overflow policy. Routing weights are normalized after scale division to avoid floating-point overflow. The immutable plan contains a model/version ticket, assignments, drop indexes and expert loads. This is **not** differentiable routing, a token activation buffer, expert execution or cross-device all-to-all.
- **NeuralTensorAdmissionBudget**: a synchronized host metadata budget reserves tensor shape byte totals plus descriptor slots atomically and releases them once through a generation-bearing Reservation. Tensor shape overflow, batch quota, concurrent overbooking and stale ticket reuse are rejected. This is **not** GPU allocation, an authenticated native TensorLease or a substitute for actor memory policy enforcement.
- **NeuralOptimizerStepLedger**: per-step in-process barrier validates distinct, expected microbatch tickets before permitting one commit transition or an abort. It fences wrong model/checkpoint/topology/request/step and duplicate gradient notifications. A training ticket is **not proof a gradient was computed**: caller must authenticate the completed gradient and native optimizer must update weights, slots, RNG and checkpoint atomically. This ledger alone cannot guarantee exactly-once behavior across process restart.

The first Java pre-dispatch ActorMailboxMiddlewareBridge is narrowed to **bounded string-to-string cleanup**, checking delegate output and every intermediate stage's original sender/recipient/sequence and value type/length before forwarding. The lower-level trusted prepareActorMail hook additionally supports bounded immutable primitive replacement with runtime validation, but no async guest continuation or untrusted host callback registration.

Do not mark MoE training, native tensor execution, the 3-actor Transformer or GPU performance as verified because these host primitives exist. Use ACT-145 through ACT-160 as future cross-backend conformance targets, still UNVERIFIED.
