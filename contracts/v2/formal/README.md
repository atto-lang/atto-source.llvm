# Bounded formal single-flow pipeline model

\`LinearPipelineSingleFlow.tla\` is a TLC-executable safety model of the
fixed root → source → transform → sink → transform → source admission
protocol currently used by the initial Java/GraalVM and LLVM pipeline slices.

The model distinguishes a **reservation** from the subsequent mailbox
**admission**, models terminal rejection, and admits source completion only
after five ordered admissions. Every emitted directional hop has one owned
reservation. The intentionally unsafe \`DuplicateCompletion\` mutation must
violate \`NoDoubleAdmission\`.

This is **model checking**, not a machine-checked implementation refinement.
It does not cover cancellation continuations, actor incarnation ABA, partial
network delivery, branching graphs, Future admission backpressure, or
multiple concurrent pipeline flows. Those require larger models, extra
implementation invariants, and integration tests on both backends.
