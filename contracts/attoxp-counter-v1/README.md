# Independently authored AttoXP LLVM scalar guest parity

These are native AttoLang `.atto` compiler fixtures, not copies of the
private `atto-xp` source repositories. The expected native exit status
of each positive program is 42. Unlike a checked-source-only pass, the
CMake `oreslang_contract_test(... --run 42 --clang ...)` pipeline calls
`oreslang-llvmc`, emits LLVM IR, links using Clang and runs the guest.

Three negative sources must produce nonzero compiler exit and no emitted
IR stdout. They test removed `%`, unresolved function and wrong arity.
Do not call generic rejection a proof of exact diagnostic parity.

These candidates establish **scalar** counter semantics only. They do
not invoke the AttoXP C ABI, create a scene, dispatch platform UI, handle
actors, or prove shared compiler parity across Java/Wasm/.NET/Forge.
Do not mark parity PASS until exact-head stepful compiler/Clang/guest
execution logs exist; source-org GitHub Actions can fail pre-run admission.
